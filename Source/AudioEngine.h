#pragma once

#include <atomic>
#include <memory>
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

    int getNumSamplesPerBlock() const { return samplesPerBlock.load(); }
    double getSampleRate() const { return sourceSampleRate.load(); }
    juce::int64 getTotalNumSamples() const { return totalNumSamples.load(); }

    // A mip-style pyramid of the analysis blocks: level 0 is the finest
    // (one entry per analysed block), and each subsequent level halves the
    // block count by reducing (min/max/energy) adjacent pairs. Crucially,
    // every level's bucket boundaries are fixed to absolute sample position
    // (anchored at sample 0) rather than depending on the current view, so
    // picking a coarser level to render a zoomed-out view doesn't cause the
    // aggregated peaks to jitter as the view pans/zooms by fractional
    // amounts - only whole blocks enter/leave at the edges.
    using MipPyramid = std::vector<std::vector<WaveformBlock>>;

    // Returns a shared_ptr to the whole pyramid rather than a reference to
    // one level, so a caller on ANY thread (in particular, WaveformComponent's
    // background texture-build thread) can hold it for the duration of a
    // read and be guaranteed a self-consistent, never-mutated-under-it
    // pyramid - even if loadFile() replaces mipLevelsPtr with a new one for
    // a different file on the message thread at the same moment. The old
    // pyramid simply stays alive (owned by the shared_ptr the reader is
    // holding) until that read finishes, instead of the reader racing a
    // vector being cleared/rebuilt out from under it.
    std::shared_ptr<const MipPyramid> getMipPyramid() const { return std::atomic_load(&mipLevelsPtr); }

    // loadFile() bumps this every time it (re)publishes mipLevelsPtr, so a
    // long-lived cache keyed off the pyramid's contents (e.g. the texture
    // tile cache) can tell a genuinely new file apart from just another read
    // of the same one, without needing to compare pyramid contents itself.
    int getLoadGeneration() const { return loadGeneration.load(); }

    juce::AudioDeviceManager& getDeviceManager() { return deviceManager; }

private:
    void analyse(juce::AudioFormatReader& reader, MipPyramid& outPyramid);
    void buildMipLevels(MipPyramid& pyramid);

    juce::AudioFormatManager formatManager;
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    // Published atomically (see getMipPyramid) each loadFile(); never
    // mutated in place after publishing, so any thread holding a copy of
    // the shared_ptr can read through it freely without locking.
    std::shared_ptr<const MipPyramid> mipLevelsPtr;
    std::atomic<int> loadGeneration { 0 };

    std::atomic<int> samplesPerBlock { 512 };
    std::atomic<double> sourceSampleRate { 44100.0 };
    std::atomic<juce::int64> totalNumSamples { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};
