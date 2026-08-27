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
    addAndMakeVisible(zoomLabel);

    fileLabel.setJustificationType(juce::Justification::centredLeft);
    frameTimeLabel.setJustificationType(juce::Justification::topRight);
    frameTimeLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey.withAlpha(0.8f));
    frameTimeLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.4f));
    frameTimeLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    frameTimeLabel.setMinimumHorizontalScale(1.0f);
    frameTimeLabel.setText("-- ms", juce::dontSendNotification);

    zoomLabel.setJustificationType(juce::Justification::topRight);
    zoomLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey.withAlpha(0.8f));
    zoomLabel.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.4f));
    zoomLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    zoomLabel.setMinimumHorizontalScale(1.0f);
    zoomLabel.setText("--", juce::dontSendNotification);

    openButton.onClick = [this] { openFileChooser(); };
    playButton.onClick = [this] { audioEngine.play(); };
    stopButton.onClick = [this] { audioEngine.stop(); };
    followPlayheadButton.onClick = [this] { waveform.setFollowPlayhead(followPlayheadButton.getToggleState()); };

    followPlayheadButton.setToggleState(true, juce::dontSendNotification);
    waveform.setFollowPlayhead(true);

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

    auto paramsArea = area.removeFromRight(340);
    parametersPanel.setBounds(paramsArea);

    auto scrollbarArea = area.removeFromBottom(20).reduced(4, 2);
    scrollbar.setBounds(scrollbarArea);

    waveform.setBounds(area.reduced(4));

    // Bottom-right corner of the whole window, overlaid on top of whatever
    // else is there (params panel / scrollbar). Tall enough for the two
    // lines of draw-call/frame timing stats.
    auto fullBounds = getLocalBounds();
    frameTimeLabel.setBounds(fullBounds.removeFromBottom(40).removeFromRight(480));
    zoomLabel.setBounds(fullBounds.removeFromBottom(106).removeFromRight(480));
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

        // Default zoom: 125 samples/px, matching the integer samples-per-
        // pixel steps the mouse wheel now zooms in (see
        // WaveformComponent::mouseWheelMove).
        constexpr double defaultSamplesPerPixel = 125.0;
        const double defaultViewLength = juce::jlimit(64.0, (double) audioEngine.getTotalNumSamples(),
                                                        defaultSamplesPerPixel * (double) juce::jmax(1, waveform.getWidth()));
        waveform.setViewRange(0.0, defaultViewLength);
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

    // samplesPerPixel is the same "how much audio does one screen pixel
    // cover" quantity WaveformComponent itself uses to decide when to
    // switch to the raw-sample fallback (see uploadWaveformTexture) - shown
    // alongside the base mip block size so it's obvious from this readout
    // alone whether the raw fallback is currently engaged.
    // Must match uploadWaveformTexture's own pixelWidth EXACTLY (getWidth()
    // times the GL rendering scale, ceil'd to an int) - omitting the
    // rendering scale here meant this readout disagreed with the actual
    // internal raw-fallback decision on any display that isn't running at
    // exactly 100% scale, which is what made "Raw fallback: off" untrustworthy.
    const double viewLengthSamples = waveform.getViewLength();
    const double pixelWidth = juce::jmax(1.0, std::ceil((double) waveform.getWidth() * waveform.getRenderingScale()));
    const double samplesPerPixel = viewLengthSamples / pixelWidth;
    const double sampleRate = audioEngine.getSampleRate();
    const int baseSamplesPerBlock = audioEngine.getNumSamplesPerBlock();
    const bool rawFallbackActive = samplesPerPixel < baseSamplesPerBlock * 0.9;

    // Diagnostics for the horizontal-jump bug: consumePeakViewJumpPx()
    // resets after each read, so this catches and holds the single largest
    // per-frame jump in the pixel-snapped view position since the last
    // timer tick (33ms), even if the actual offending frame falls between
    // ticks.
    const double peakJumpPx = waveform.consumePeakViewJumpPx();
    const double peakCorrectionSec = waveform.consumePeakTrackerCorrectionSeconds();
    const int snapCount = waveform.consumeSnapCount();
    const int notPlayingResetCount = waveform.consumeNotPlayingResetCount();
    const bool actualRawFallback = waveform.getLastRawFallbackActive();
    const int rawFallbackFlips = waveform.consumeRawFallbackFlipCount();

    juce::String zoomText;
    zoomText << "Zoom: " << juce::String(samplesPerPixel, 3) << " samples/px"
              << "  (view " << juce::String(viewLengthSamples, 0) << " samples"
              << (sampleRate > 0.0 ? ", " + juce::String(viewLengthSamples / sampleRate, 3) + " s)" : ")")
              << "\nRaw fallback: " << (rawFallbackActive ? "ON" : "off") << " (label calc)"
              << " / " << (actualRawFallback ? "ON" : "off") << " (actual)"
              << "  Flips: " << rawFallbackFlips
              << "\n(mip block = " << baseSamplesPerBlock << " samples)"
              << "\nView px: " << juce::String(waveform.getLastSnappedViewStartPx(), 1)
              << "  Peak jump: " << juce::String(peakJumpPx, 2) << " px"
              << "\nTracker correction: " << juce::String(peakCorrectionSec * 1000.0, 2) << " ms"
              << "  Snaps: " << snapCount
              << "  NotPlayingResets: " << notPlayingResetCount
              << "\nTexWin offset: " << juce::String(waveform.getLastTextureViewStartOffsetPx(), 1) << " px"
              << "  Peak scale jump: " << juce::String(waveform.consumePeakTextureScaleJumpPct(), 2) << " %";
    zoomLabel.setText(zoomText, juce::dontSendNotification);
}
