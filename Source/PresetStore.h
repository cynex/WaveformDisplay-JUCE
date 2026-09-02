#pragma once

#include <JuceHeader.h>
#include "WaveformParameters.h"

/** Persists named waveform parameter snapshots in settings.json beside the
    running executable. */
class PresetStore
{
public:
    PresetStore();

    bool reload(juce::String& errorMessage);
    bool savePreset(const juce::String& name,
                    const WaveformParameters& parameters,
                    juce::String& errorMessage);
    bool recallPreset(const juce::String& name, WaveformParameters& parameters) const;
    bool rememberLastPreset(const juce::String& name, juce::String& errorMessage);

    juce::StringArray getPresetNames() const;
    const juce::String& getLastPresetName() const { return lastPresetName; }
    const juce::File& getSettingsFile() const { return settingsFile; }

private:
    struct Entry
    {
        juce::String name;
        WaveformParameters parameters;
    };

    int findPreset(const juce::String& name) const;
    bool write(juce::String& errorMessage) const;

    juce::File settingsFile;
    std::vector<Entry> entries;
    juce::String lastPresetName;
};
