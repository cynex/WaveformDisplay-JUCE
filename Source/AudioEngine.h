#pragma once

#include <JuceHeader.h>

/** One analysed block of audio: peak envelope plus a rough split of
    energy into low/mid/high-frequency bands. All fields are normalised to
    sensible ranges for the waveform shader:
        min/max        -1..1   (sample amplitude envelope)
        lowEnergy       0..1   (relative low-band energy in this block)
        midEnergy       0..1   (relative mid-band energy in this block)
        highEnergy      0..1   (relative high-band energy in this block)
*/
struct WaveformBlock
{
    float minValue = 0.0f;
    float maxValue = 0.0f;
    float lowEnergy = 0.0f;
    float midEnergy = 0.0f;
    float highEnergy = 0.0f;
};

/** Loads an audio file (wav/flac/ogg/mp3/aiff - whatever the registered
    JUCE AudioFormats support), plays it back through the default audio
    device, and produces a per-block analysis buffer used to drive the
    SDF waveform renderer. */
class AudioEngine : private juce::ChangeBroadcaster
{
public:
    AudioEngine();
    ~AudioEngine() override;

    bool loadFile(const juce::File& file);

    void play();
    void stop();
    bool isPlaying() const;

    void setPosition(double seconds);
    double getPosition() const;
    double getLengthInSeconds() const;
    bool hasFileLoaded() const { return readerSource != nullptr; }

    juce::AudioTransportSource& getTransportSource() { return transportSource; }

    int getNumSamplesPerBlock() const { return samplesPerBlock; }
    double getSampleRate() const { return sourceSampleRate; }
    juce::int64 getTotalNumSamples() const { return totalNumSamples; }

    // A mip-style pyramid of the analysis blocks: level 0 is the finest
    // (one entry per analysed block), and each subsequent level halves the
    // block count by reducing (min/max/energy) adjacent pairs. Crucially,
    // every level's bucket boundaries are fixed to absolute sample position
    // (anchored at sample 0) rather than depending on the current view, so
    // picking a coarser level to render a zoomed-out view doesn't cause the
    // aggregated peaks to jitter as the view pans/zooms by fractional
    // amounts - only whole blocks enter/leave at the edges.
    int getNumMipLevels() const { return (int) mipLevels.size(); }
    const std::vector<WaveformBlock>& getMipLevel(int level) const
    {
        static const std::vector<WaveformBlock> empty;
        if (mipLevels.empty())
            return empty;
        return mipLevels[(size_t) juce::jlimit(0, (int) mipLevels.size() - 1, level)];
    }

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

    // Computes numBuckets WaveformBlocks directly from raw decoded samples
    // spanning [startSample, startSample + spanSamples), rather than from
    // the fixed-size mip pyramid - used when the view is zoomed in far
    // enough that even mip level 0's fixed block size is coarser than one
    // screen pixel, so the pyramid alone can't show any more detail. Uses a
    // reader instance dedicated to this (see rawReader below), independent
    // of the one driving playback, so a lookup here (called from the GL
    // thread) never races the audio thread's own decode calls. Returns
    // false if there's no file loaded, or the range/bucket count is
    // degenerate.
    bool getRawBlocks(juce::int64 startSample, juce::int64 spanSamples, int numBuckets, std::vector<WaveformBlock>& outBlocks) const;

private:
    void analyse(juce::AudioFormatReader& reader);
    void buildMipLevels();

    juce::AudioFormatManager formatManager;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    // Dedicated to getRawBlocks() above - deliberately a second, independent
    // reader rather than reusing readerSource's, so on-demand lookups from
    // the GL thread can't race the audio thread's decode calls on the same
    // AudioFormatReader instance during playback.
    std::unique_ptr<juce::AudioFormatReader> rawReader;

    std::vector<std::vector<WaveformBlock>> mipLevels;
    int samplesPerBlock = 512;
    double sourceSampleRate = 44100.0;
    juce::int64 totalNumSamples = 0;

    // Whole-file normalisation factors computed once in analyse() and
    // reapplied by getRawBlocks(), so raw-sample-fallback blocks are scaled
    // identically to the mip pyramid's blocks instead of looking
    // inconsistently louder/dimmer right at the zoom level where the
    // fallback kicks in.
    float normMaxLowEnergy = 0.0001f;
    float normMaxMidEnergy = 0.0001f;
    float normMaxHighEnergy = 0.0001f;
    float normPeakScaleDivisor = 1.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
