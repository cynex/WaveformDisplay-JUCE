#include "AudioEngine.h"

AudioEngine::AudioEngine()
{
    formatManager.registerBasicFormats(); // wav, aiff, flac, ogg vorbis, mp3 (decode-only)

    deviceManager.initialiseWithDefaultDevices(0, 2);
    deviceManager.addAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(&transportSource);
}

AudioEngine::~AudioEngine()
{
    audioSourcePlayer.setSource(nullptr);
    deviceManager.removeAudioCallback(&audioSourcePlayer);
    transportSource.setSource(nullptr);
    readerSource.reset();
}

bool AudioEngine::loadFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
        return false;

    sourceSampleRate = reader->sampleRate;
    totalNumSamples = reader->lengthInSamples;

    analyse(*reader);

    // A second, independent reader for the same file - see the member
    // comment on rawReader for why this can't just reuse readerSource's.
    rawReader.reset(formatManager.createReaderFor(file));

    auto newSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);
    transportSource.setSource(newSource.get(), 0, nullptr, sourceSampleRate);
    readerSource = std::move(newSource);

    return true;
}

void AudioEngine::analyse(juce::AudioFormatReader& reader)
{
    mipLevels.clear();
    std::vector<WaveformBlock> blocks;

    const juce::int64 numSamples = reader.lengthInSamples;
    if (numSamples <= 0)
    {
        mipLevels.push_back({});
        return;
    }

    // Aim for roughly 200k analysis blocks max for very long files, otherwise
    // keep a fine fixed block size for good zoom resolution on typical files.
    samplesPerBlock = (int) juce::jmax<juce::int64>(64, numSamples / 200000);
    const int numBlocks = (int) ((numSamples + samplesPerBlock - 1) / samplesPerBlock);
    blocks.resize((size_t) numBlocks);

    const int numChannels = (int) reader.numChannels;
    juce::AudioBuffer<float> buffer(numChannels, samplesPerBlock);

    // Two cascaded one-pole low-pass filters split the signal into three
    // bands: low (below cutoffLowHz), mid (between the two cutoffs), and
    // high (above cutoffHighHz) - lowLpState tracks the low band directly,
    // midLpState tracks a wider low-pass whose difference from lowLpState
    // is the mid band, and whatever's left after removing both is the high
    // band.
    float lowLpState = 0.0f;
    float midLpState = 0.0f;

    const float cutoffLowHz = 300.0f;
    const float cutoffHighHz = 3000.0f;
    const float dt = 1.0f / (float) sourceSampleRate;
    const float rcLow = 1.0f / (2.0f * juce::MathConstants<float>::pi * cutoffLowHz);
    const float rcHigh = 1.0f / (2.0f * juce::MathConstants<float>::pi * cutoffHighHz);
    const float alphaLow = dt / (rcLow + dt);
    const float alphaHigh = dt / (rcHigh + dt);

    juce::int64 samplePos = 0;
    for (int b = 0; b < numBlocks; ++b)
    {
        const int thisBlockSize = (int) juce::jmin<juce::int64>(samplesPerBlock, numSamples - samplePos);
        reader.read(&buffer, 0, thisBlockSize, samplePos, true, true);

        float minV = 0.0f, maxV = 0.0f;
        float lowSumSq = 0.0f, midSumSq = 0.0f, highSumSq = 0.0f;

        for (int i = 0; i < thisBlockSize; ++i)
        {
            float sample = 0.0f;
            for (int ch = 0; ch < numChannels; ++ch)
                sample += buffer.getSample(ch, i);
            sample /= (float) juce::jmax(1, numChannels);

            // Guard against occasional decoder garbage (e.g. codec priming
            // samples/edge-of-stream artifacts) producing a wild outlier that
            // would otherwise dominate the whole-file peak used to normalise
            // the envelope, crushing all genuine audio down near zero.
            sample = juce::jlimit(-4.0f, 4.0f, sample);

            minV = juce::jmin(minV, sample);
            maxV = juce::jmax(maxV, sample);

            lowLpState += alphaLow * (sample - lowLpState);
            midLpState += alphaHigh * (sample - midLpState);
            const float midVal = midLpState - lowLpState;
            const float highVal = sample - midLpState;

            lowSumSq += lowLpState * lowLpState;
            midSumSq += midVal * midVal;
            highSumSq += highVal * highVal;
        }

        WaveformBlock wb;
        wb.minValue = minV;
        wb.maxValue = maxV;
        wb.lowEnergy = thisBlockSize > 0 ? std::sqrt(lowSumSq / (float) thisBlockSize) : 0.0f;
        wb.midEnergy = thisBlockSize > 0 ? std::sqrt(midSumSq / (float) thisBlockSize) : 0.0f;
        wb.highEnergy = thisBlockSize > 0 ? std::sqrt(highSumSq / (float) thisBlockSize) : 0.0f;
        blocks[(size_t) b] = wb;

        samplePos += thisBlockSize;
    }

    // Normalise low/mid/high energy across the whole file to 0..1 for
    // consistent tinting. Stashed on the instance too so getRawBlocks() can
    // apply the identical scale to its on-demand blocks.
    float maxLow = 0.0001f, maxMid = 0.0001f, maxHigh = 0.0001f;
    for (auto& wb : blocks)
    {
        maxLow = juce::jmax(maxLow, wb.lowEnergy);
        maxMid = juce::jmax(maxMid, wb.midEnergy);
        maxHigh = juce::jmax(maxHigh, wb.highEnergy);
    }
    for (auto& wb : blocks)
    {
        wb.lowEnergy = juce::jlimit(0.0f, 1.0f, wb.lowEnergy / maxLow);
        wb.midEnergy = juce::jlimit(0.0f, 1.0f, wb.midEnergy / maxMid);
        wb.highEnergy = juce::jlimit(0.0f, 1.0f, wb.highEnergy / maxHigh);
    }
    normMaxLowEnergy = maxLow;
    normMaxMidEnergy = maxMid;
    normMaxHighEnergy = maxHigh;

    // Normalise the peak envelope to -1..1 too. Some readers/formats don't
    // hand back samples in a clean -1..1 range (e.g. slightly hot masters,
    // or fixed-point sources that overshoot after conversion), and the
    // waveform texture packs min/max into an 8-bit channel assuming -1..1 -
    // anything outside that gets silently clipped to the edge, which looks
    // like the whole waveform collapsing to a flat line pinned at ±1.
    float peakAbs = 0.0001f;
    for (auto& wb : blocks)
        peakAbs = juce::jmax(peakAbs, std::abs(wb.minValue), std::abs(wb.maxValue));

    // Cap how far a single outlier block can drag the normalisation scale -
    // a rare spike should get clipped at +/-1 rather than being allowed to
    // crush every other (genuinely -1..1) block down towards silence.
    const float scaleDivisor = juce::jmin(peakAbs, 2.0f);

    normPeakScaleDivisor = 1.0f;
    if (peakAbs > 1.05f || peakAbs < 0.5f)
    {
        normPeakScaleDivisor = scaleDivisor;
        for (auto& wb : blocks)
        {
            wb.minValue = juce::jlimit(-1.0f, 1.0f, wb.minValue / scaleDivisor);
            wb.maxValue = juce::jlimit(-1.0f, 1.0f, wb.maxValue / scaleDivisor);
        }
    }

    mipLevels.push_back(std::move(blocks));
    buildMipLevels();
}

void AudioEngine::buildMipLevels()
{
    // Keep halving until the level is small enough to always fit inside a
    // texture, however far the user zooms out. Bucket boundaries at every
    // level are fixed to absolute sample position (pairs of the previous
    // level, starting from index 0) rather than anything view-dependent -
    // that's what keeps the rendered peaks from jittering as the view pans
    // or zooms by fractional amounts.
    constexpr size_t smallestLevelSize = 1024;

    while (mipLevels.back().size() > smallestLevelSize)
    {
        const auto& prev = mipLevels.back();
        std::vector<WaveformBlock> next((prev.size() + 1) / 2);

        for (size_t i = 0; i < next.size(); ++i)
        {
            WaveformBlock wb = prev[i * 2];
            if (i * 2 + 1 < prev.size())
            {
                const auto& other = prev[i * 2 + 1];
                wb.minValue = juce::jmin(wb.minValue, other.minValue);
                wb.maxValue = juce::jmax(wb.maxValue, other.maxValue);
                wb.lowEnergy = juce::jmax(wb.lowEnergy, other.lowEnergy);
                wb.midEnergy = juce::jmax(wb.midEnergy, other.midEnergy);
                wb.highEnergy = juce::jmax(wb.highEnergy, other.highEnergy);
            }
            next[i] = wb;
        }

        mipLevels.push_back(std::move(next));
    }
}

void AudioEngine::play()
{
    if (readerSource != nullptr)
        transportSource.start();
}

void AudioEngine::stop()
{
    transportSource.stop();
}

bool AudioEngine::isPlaying() const
{
    return transportSource.isPlaying();
}

void AudioEngine::setPosition(double seconds)
{
    transportSource.setPosition(seconds);
}

double AudioEngine::getPosition() const
{
    return transportSource.getCurrentPosition();
}

double AudioEngine::getLengthInSeconds() const
{
    return transportSource.getLengthInSeconds();
}

bool AudioEngine::getRawBlocks(juce::int64 startSample, juce::int64 spanSamples, int numBuckets, std::vector<WaveformBlock>& outBlocks) const
{
    if (rawReader == nullptr || numBuckets <= 0 || spanSamples <= 0 || totalNumSamples <= 0)
        return false;

    startSample = juce::jlimit<juce::int64>(0, juce::jmax<juce::int64>(0, totalNumSamples - 1), startSample);
    spanSamples = juce::jmin(spanSamples, totalNumSamples - startSample);
    if (spanSamples <= 0)
        return false;

    // Extra samples read BEFORE startSample purely to warm up the one-pole
    // filters below (see the loop) - never turned into bucket data
    // themselves. analyse() runs its identical filters continuously across
    // the WHOLE file, so its energy values reflect fully-settled filter
    // state everywhere; here each rebuild instead starts a fresh window from
    // startSample with the filters reset to 0. Right at this window's start
    // that gives a different low/mid/high split than analyse() would have
    // produced for the exact same audio - normally not visible since it
    // settles out within the margin before reaching the visible view, but
    // at deep zoom the margin itself can be just a few hundred samples,
    // less than the 300Hz low band needs to settle (~5 time constants,
    // roughly 2650 samples at 44.1kHz) - so the visible start of the window
    // showed a DIFFERENT tint each rebuild depending on how much settling
    // time that particular window's margin happened to give it, which read
    // as the whole waveform intermittently jumping/flashing. 8000 samples
    // (~180ms at 44.1kHz) comfortably covers that settling time regardless
    // of zoom.
    constexpr juce::int64 filterPrerollSamples = 8000;
    const juce::int64 prerollStart = juce::jmax<juce::int64>(0, startSample - filterPrerollSamples);
    const juce::int64 prerollCount = startSample - prerollStart;
    const juce::int64 readCount = prerollCount + spanSamples;

    const int numChannels = juce::jmax(1, (int) rawReader->numChannels);
    juce::AudioBuffer<float> buffer(numChannels, (int) readCount);
    if (! rawReader->read(&buffer, 0, (int) readCount, prerollStart, true, true))
        return false;

    outBlocks.assign((size_t) numBuckets, WaveformBlock());

    // Same three-band one-pole split as analyse() - see the comment there -
    // run continuously across the whole span (not reset per bucket) so the
    // filter state, and so the resulting energy split, matches what
    // analyse() would have produced for this same stretch of audio.
    float lowLpState = 0.0f;
    float midLpState = 0.0f;
    const float cutoffLowHz = 300.0f;
    const float cutoffHighHz = 3000.0f;
    const float dt = 1.0f / (float) sourceSampleRate;
    const float rcLow = 1.0f / (2.0f * juce::MathConstants<float>::pi * cutoffLowHz);
    const float rcHigh = 1.0f / (2.0f * juce::MathConstants<float>::pi * cutoffHighHz);
    const float alphaLow = dt / (rcLow + dt);
    const float alphaHigh = dt / (rcHigh + dt);

    std::vector<float> lowSumSq((size_t) numBuckets, 0.0f);
    std::vector<float> midSumSq((size_t) numBuckets, 0.0f);
    std::vector<float> highSumSq((size_t) numBuckets, 0.0f);
    std::vector<int> bucketCount((size_t) numBuckets, 0);

    for (juce::int64 i = 0; i < readCount; ++i)
    {
        float sample = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
            sample += buffer.getSample(ch, (int) i);
        sample /= (float) numChannels;
        sample = juce::jlimit(-4.0f, 4.0f, sample);

        lowLpState += alphaLow * (sample - lowLpState);
        midLpState += alphaHigh * (sample - midLpState);
        const float midVal = midLpState - lowLpState;
        const float highVal = sample - midLpState;

        if (i < prerollCount)
            continue; // warm-up only - not part of any output bucket

        const juce::int64 spanIndex = i - prerollCount;
        int bucket = (int) ((double) spanIndex * (double) numBuckets / (double) spanSamples);
        bucket = juce::jlimit(0, numBuckets - 1, bucket);

        auto& wb = outBlocks[(size_t) bucket];
        auto& count = bucketCount[(size_t) bucket];
        if (count == 0)
        {
            wb.minValue = sample;
            wb.maxValue = sample;
        }
        else
        {
            wb.minValue = juce::jmin(wb.minValue, sample);
            wb.maxValue = juce::jmax(wb.maxValue, sample);
        }
        lowSumSq[(size_t) bucket] += lowLpState * lowLpState;
        midSumSq[(size_t) bucket] += midVal * midVal;
        highSumSq[(size_t) bucket] += highVal * highVal;
        ++count;
    }

    // Apply the SAME whole-file normalisation analyse() used, so these
    // on-demand blocks read consistently with the rest of the mip pyramid
    // instead of jumping in brightness/scale right at the zoom level where
    // this fallback kicks in.
    for (int b = 0; b < numBuckets; ++b)
    {
        auto& wb = outBlocks[(size_t) b];
        const int count = bucketCount[(size_t) b];
        wb.lowEnergy = count > 0 ? juce::jlimit(0.0f, 1.0f, std::sqrt(lowSumSq[(size_t) b] / (float) count) / normMaxLowEnergy) : 0.0f;
        wb.midEnergy = count > 0 ? juce::jlimit(0.0f, 1.0f, std::sqrt(midSumSq[(size_t) b] / (float) count) / normMaxMidEnergy) : 0.0f;
        wb.highEnergy = count > 0 ? juce::jlimit(0.0f, 1.0f, std::sqrt(highSumSq[(size_t) b] / (float) count) / normMaxHighEnergy) : 0.0f;
        wb.minValue = juce::jlimit(-1.0f, 1.0f, wb.minValue / normPeakScaleDivisor);
        wb.maxValue = juce::jlimit(-1.0f, 1.0f, wb.maxValue / normPeakScaleDivisor);
    }

    return true;
}
