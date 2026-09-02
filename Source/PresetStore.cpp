#include "PresetStore.h"

#include <algorithm>

namespace
{
    juce::var parametersToVar(const WaveformParameters& p)
    {
        auto object = std::make_unique<juce::DynamicObject>();

        object->setProperty("solidColour", p.solidColour.toString());
        object->setProperty("lowFreqColour", p.lowFreqColour.toString());
        object->setProperty("midFreqColour", p.midFreqColour.toString());
        object->setProperty("highFreqColour", p.highFreqColour.toString());
        object->setProperty("amplitudeLowColour", p.amplitudeLowColour.toString());
        object->setProperty("amplitudeHighColour", p.amplitudeHighColour.toString());
        object->setProperty("backgroundColour", p.backgroundColour.toString());
        object->setProperty("playheadColour", p.playheadColour.toString());

        object->setProperty("lowFreqAmount", p.lowFreqAmount);
        object->setProperty("midFreqAmount", p.midFreqAmount);
        object->setProperty("highFreqAmount", p.highFreqAmount);
        object->setProperty("waveformHeight", p.waveformHeight);
        object->setProperty("amplitudeAmount", p.amplitudeAmount);
        object->setProperty("amplitudeColorAmount", p.amplitudeColorAmount);
        object->setProperty("amplitudeGlowRadius", p.amplitudeGlowRadius);
        object->setProperty("amplitudeRange", p.amplitudeRange);
        object->setProperty("amplitudeSlope", p.amplitudeSlope);
        object->setProperty("amplitudeMinFrequencyHz", p.amplitudeMinFrequencyHz);
        object->setProperty("amplitudeMaxFrequencyHz", p.amplitudeMaxFrequencyHz);
        object->setProperty("midPole", p.midPole);
        object->setProperty("tintingEnabled", p.tintingEnabled);
        object->setProperty("aaWidth", p.aaWidth);
        object->setProperty("smoothing", p.smoothing);
        object->setProperty("centreLineAlpha", p.centreLineAlpha);

        return juce::var(object.release());
    }

    float readFloat(const juce::DynamicObject& object,
                    const juce::Identifier& key,
                    float fallback,
                    float minimum,
                    float maximum)
    {
        if (!object.hasProperty(key))
            return fallback;

        const auto value = object.getProperty(key);
        if (!value.isDouble() && !value.isInt() && !value.isInt64())
            return fallback;

        return juce::jlimit(minimum, maximum, static_cast<float>(static_cast<double>(value)));
    }

    bool readBool(const juce::DynamicObject& object, const juce::Identifier& key, bool fallback)
    {
        const auto value = object.getProperty(key);
        return object.hasProperty(key) && value.isBool() ? static_cast<bool>(value) : fallback;
    }

    juce::Colour readColour(const juce::DynamicObject& object,
                            const juce::Identifier& key,
                            juce::Colour fallback)
    {
        const auto value = object.getProperty(key);
        if (!object.hasProperty(key) || !value.isString() || value.toString().isEmpty())
            return fallback;

        return juce::Colour::fromString(value.toString());
    }

    WaveformParameters parametersFromVar(const juce::var& value)
    {
        WaveformParameters p;
        auto* object = value.getDynamicObject();
        if (object == nullptr)
            return p;

        p.solidColour = readColour(*object, "solidColour", p.solidColour);
        p.lowFreqColour = readColour(*object, "lowFreqColour", p.lowFreqColour);
        p.midFreqColour = readColour(*object, "midFreqColour", p.midFreqColour);
        p.highFreqColour = readColour(*object, "highFreqColour", p.highFreqColour);
        p.amplitudeLowColour = readColour(*object, "amplitudeLowColour", p.amplitudeLowColour);
        p.amplitudeHighColour = readColour(*object, "amplitudeHighColour", p.amplitudeHighColour);
        p.backgroundColour = readColour(*object, "backgroundColour", p.backgroundColour);
        p.playheadColour = readColour(*object, "playheadColour", p.playheadColour);

        p.lowFreqAmount = readFloat(*object, "lowFreqAmount", p.lowFreqAmount, 0.0f, 1.0f);
        p.midFreqAmount = readFloat(*object, "midFreqAmount", p.midFreqAmount, 0.0f, 1.0f);
        p.highFreqAmount = readFloat(*object, "highFreqAmount", p.highFreqAmount, 0.0f, 1.0f);
        p.waveformHeight = readFloat(*object, "waveformHeight", p.waveformHeight, 0.1f, 1.0f);
        p.amplitudeAmount = readFloat(*object, "amplitudeAmount", p.amplitudeAmount, 0.0f, 1.0f);
        p.amplitudeColorAmount = readFloat(*object, "amplitudeColorAmount", p.amplitudeColorAmount, 0.0f, 4.0f);
        p.amplitudeGlowRadius = readFloat(*object, "amplitudeGlowRadius", p.amplitudeGlowRadius, 0.0f, 250.0f);
        p.amplitudeRange = readFloat(*object, "amplitudeRange", p.amplitudeRange, 0.0001f, 0.25f);
        p.amplitudeSlope = readFloat(*object, "amplitudeSlope", p.amplitudeSlope, 1.0f, 12.0f);
        p.amplitudeMinFrequencyHz = readFloat(*object, "amplitudeMinFrequencyHz", p.amplitudeMinFrequencyHz, 20.0f, 20000.0f);
        p.amplitudeMaxFrequencyHz = readFloat(*object, "amplitudeMaxFrequencyHz", p.amplitudeMaxFrequencyHz, 20.0f, 20000.0f);
        p.midPole = readFloat(*object, "midPole", p.midPole, 0.01f, 0.99f);
        p.tintingEnabled = readBool(*object, "tintingEnabled", p.tintingEnabled);
        p.aaWidth = readFloat(*object, "aaWidth", p.aaWidth, 0.1f, 8.0f);
        p.smoothing = readFloat(*object, "smoothing", p.smoothing, 0.0f, 1.0f);
        p.centreLineAlpha = readFloat(*object, "centreLineAlpha", p.centreLineAlpha, 0.0f, 1.0f);
        return p;
    }
}

PresetStore::PresetStore()
    : settingsFile(juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                       .getParentDirectory()
                       .getChildFile("settings.json"))
{
}

bool PresetStore::reload(juce::String& errorMessage)
{
    entries.clear();
    lastPresetName.clear();
    errorMessage.clear();

    if (!settingsFile.existsAsFile())
        return true;

    juce::var rootValue;
    auto result = juce::JSON::parse(settingsFile.loadFileAsString(), rootValue);
    if (result.failed())
    {
        errorMessage = "Could not parse " + settingsFile.getFullPathName() + ":\n" + result.getErrorMessage();
        return false;
    }

    auto* root = rootValue.getDynamicObject();
    if (root == nullptr)
    {
        errorMessage = "The preset settings file does not contain a JSON object.";
        return false;
    }

    lastPresetName = root->getProperty("lastPreset").toString();
    const auto presetsValue = root->getProperty("presets");
    if (auto* presets = presetsValue.getArray())
    {
        for (const auto& presetValue : *presets)
        {
            auto* presetObject = presetValue.getDynamicObject();
            if (presetObject == nullptr)
                continue;

            auto name = presetObject->getProperty("name").toString().trim();
            if (name.isEmpty() || findPreset(name) >= 0)
                continue;

            entries.push_back({ name, parametersFromVar(presetObject->getProperty("parameters")) });
        }
    }

    return true;
}

bool PresetStore::savePreset(const juce::String& name,
                             const WaveformParameters& parameters,
                             juce::String& errorMessage)
{
    auto cleanName = name.trim();
    if (cleanName.isEmpty())
    {
        errorMessage = "Enter a preset name before saving.";
        return false;
    }

    const auto index = findPreset(cleanName);
    if (index >= 0)
    {
        entries[static_cast<size_t>(index)].name = cleanName;
        entries[static_cast<size_t>(index)].parameters = parameters;
    }
    else
    {
        entries.push_back({ cleanName, parameters });
    }

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b)
    {
        return a.name.compareIgnoreCase(b.name) < 0;
    });

    lastPresetName = cleanName;
    return write(errorMessage);
}

bool PresetStore::recallPreset(const juce::String& name, WaveformParameters& parameters) const
{
    const auto index = findPreset(name.trim());
    if (index < 0)
        return false;

    parameters = entries[static_cast<size_t>(index)].parameters;
    return true;
}

bool PresetStore::rememberLastPreset(const juce::String& name, juce::String& errorMessage)
{
    lastPresetName = name.trim();
    return write(errorMessage);
}

juce::StringArray PresetStore::getPresetNames() const
{
    juce::StringArray names;
    for (const auto& entry : entries)
        names.add(entry.name);
    return names;
}

int PresetStore::findPreset(const juce::String& name) const
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].name.equalsIgnoreCase(name))
            return static_cast<int>(i);

    return -1;
}

bool PresetStore::write(juce::String& errorMessage) const
{
    errorMessage.clear();

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty("version", 1);
    root->setProperty("lastPreset", lastPresetName);

    juce::Array<juce::var> presetValues;
    for (const auto& entry : entries)
    {
        auto preset = std::make_unique<juce::DynamicObject>();
        preset->setProperty("name", entry.name);
        preset->setProperty("parameters", parametersToVar(entry.parameters));
        presetValues.add(juce::var(preset.release()));
    }
    root->setProperty("presets", juce::var(presetValues));

    const auto json = juce::JSON::toString(juce::var(root.release()), false);
    if (!settingsFile.replaceWithText(json + "\n"))
    {
        errorMessage = "Could not write presets to:\n" + settingsFile.getFullPathName();
        return false;
    }

    return true;
}
