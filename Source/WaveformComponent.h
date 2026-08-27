#pragma once

#include <atomic>
#include <JuceHeader.h>
#include "AudioEngine.h"
#include "WaveformParameters.h"

/** Renders the waveform envelope of the loaded audio file using a
    signed-distance-field fragment shader for crisp, resolution-independent
    anti-aliasing regardless of zoom level.

    The min/max/low-energy/high-energy analysis produced by AudioEngine can
    have far more blocks than any GPU texture dimension allows (a full song
    is easily 100k+ analysis blocks, versus a GL_MAX_TEXTURE_SIZE of
    8192-16384). Rather than downsampling the whole file into one fixed-size
    texture once - which throws away resolution everywhere except when
    fully zoomed out - the texture is rebuilt from just the currently
    visible sample range every time the view changes. When zoomed in far
    enough that the visible range covers fewer analysis blocks than the
    texture width limit, this gives one texel per block (full source
    resolution); when zoomed out further than that, blocks are reduced
    (min/max/energy) down into the available texels, same as before.

    Mouse wheel  -> zoom (centred on the cursor position)
    Click + drag -> pan
*/
class WaveformComponent : public juce::Component,
                           public juce::OpenGLRenderer,
                           private juce::Timer
{
public:
    explicit WaveformComponent(AudioEngine& engine);
    ~WaveformComponent() override;

    void setParameters(const WaveformParameters& newParams);
    const WaveformParameters& getParameters() const { return params; }

    // View state, in samples. [viewStart, viewStart + viewLength) is
    // the range of the source file currently visible in the component.
    void setViewRange(double viewStartSamples, double viewLengthSamples);
    double getViewStart() const { const juce::ScopedLock sl(dataLock); return viewStart; }
    double getViewLength() const { const juce::ScopedLock sl(dataLock); return viewLength; }
    double getTotalLength() const;

    /** Call after loading a new file so the GPU texture is rebuilt from the new analysis data. */
    void notifyFileChanged() { textureDirty = true; }

    /** When enabled, the view continuously recentres on the playhead during playback. */
    void setFollowPlayhead(bool shouldFollow) { followPlayhead = shouldFollow; }
    bool getFollowPlayhead() const { return followPlayhead; }

    /** Wall-clock time the last GL draw call (glDrawArrays for the waveform quad) took, in milliseconds. */
    double getLastDrawCallMs() const { return lastDrawCallMs.load(); }
    /** Highest getLastDrawCallMs() has been since the app started. */
    double getPeakDrawCallMs() const { return peakDrawCallMs.load(); }

    /** Wall-clock time the last whole renderOpenGL() call took (texture rebuild when needed, plus the draw call), in milliseconds. */
    double getLastFrameMs() const { return lastFrameMs.load(); }
    /** Highest getLastFrameMs() has been since the app started. */
    double getPeakFrameMs() const { return peakFrameMs.load(); }

    std::function<void()> onViewRangeChanged;

    // juce::Component
    void resized() override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;

    // juce::OpenGLRenderer
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

private:
    void timerCallback() override;
    void uploadWaveformTexture(double viewStartSamples, double viewLengthSamples);
    void buildShaders();
    void clampView();

    // AudioEngine::getPosition() only actually CHANGES value once per audio
    // device callback buffer (commonly every ~10-30ms, sometimes more) -
    // reading it directly makes the playhead hold still and then visibly
    // jump forward by that whole chunk each time it ticks. Zoomed out, one
    // chunk is a fraction of a pixel; zoomed in close, it's a large,
    // increasingly obvious jump (the more you zoom in, the more pixels one
    // fixed-size chunk of samples covers) - which is exactly what made the
    // jump distance scale with zoom.
    //
    // This smooths that out by extrapolating continuously from the last
    // authoritative reading using wall-clock time elapsed since it arrived
    // (assuming ordinary 1x forward playback), re-anchoring the instant the
    // underlying value actually ticks to a new one - so drift can never
    // accumulate for more than a single buffer period before being
    // corrected back to ground truth. A monotonic floor additionally
    // suppresses any tiny backward wobble in the readings themselves, while
    // still passing a genuinely large backward jump (a real seek/loop)
    // through immediately.
    class SmoothPositionTracker
    {
    public:
        double update(double rawPositionSeconds, bool isPlaying)
        {
            if (!isPlaying)
            {
                valid = false;
                return rawPositionSeconds;
            }

            const double nowMs = juce::Time::getMillisecondCounterHiRes();

            if (!valid || rawPositionSeconds != lastRawSeen)
            {
                anchorPosition = rawPositionSeconds;
                anchorTimeMs = nowMs;
                lastRawSeen = rawPositionSeconds;
                valid = true;
            }

            double estimated = anchorPosition + (nowMs - anchorTimeMs) / 1000.0;

            // Don't let timing assumptions run us more than one buffer
            // period ahead of the last actual reading.
            estimated = juce::jmin(estimated, rawPositionSeconds + 0.25);

            if (lastReturnedValid)
            {
                // Suppress a small backward wobble in the readings; let a
                // large one (a real seek/loop) through immediately.
                if (estimated < lastReturnedEstimate && lastReturnedEstimate - estimated < 0.08)
                    estimated = lastReturnedEstimate;
            }

            lastReturnedEstimate = estimated;
            lastReturnedValid = true;
            return estimated;
        }

    private:
        double lastRawSeen = -1.0;
        double anchorPosition = 0.0;
        double anchorTimeMs = 0.0;
        bool valid = false;

        double lastReturnedEstimate = 0.0;
        bool lastReturnedValid = false;
    };

    AudioEngine& audioEngine;
    juce::OpenGLContext openGLContext;
    WaveformParameters params;

    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    GLuint vertexBuffer = 0;
    GLuint waveformTexture = 0;   // R=min, G=max, B=lowEnergy, A=highEnergy
    GLuint midTexture = 0;        // R=midEnergy (G/B/A unused)
    // Flipped true from the message thread (setViewRange, resized) and read
    // and cleared from the GL render thread (renderOpenGL/uploadWaveformTexture)
    // - atomic so that handoff is properly visible across threads without
    // holding dataLock for the whole (potentially non-trivial) texture rebuild.
    std::atomic<bool> textureDirty { true };
    int textureWidth = 0;
    int allocatedTextureWidth = -1; // GL-thread-only: what glTexImage2D last actually allocated for both textures

    // Wall-clock duration of the last glDrawArrays call, and the highest
    // that's ever been, for the on-screen frame-time readout. Written on
    // the GL thread, read from the message thread (MainComponent's timer)
    // - atomic<double> for safe handoff.
    std::atomic<double> lastDrawCallMs { 0.0 };
    std::atomic<double> peakDrawCallMs { 0.0 };

    // Wall-clock duration of the whole last renderOpenGL() call (texture
    // rebuild when one was needed, plus the draw call - i.e. everything
    // this class does to produce one frame), and the highest that's ever
    // been since the app started. Set via FrameTimerGuard below, which
    // times the function regardless of which of its several early returns
    // actually gets hit.
    std::atomic<double> lastFrameMs { 0.0 };
    std::atomic<double> peakFrameMs { 0.0 };

    // RAII helper: construct at the top of renderOpenGL() so its destructor
    // records the elapsed time no matter which return statement is taken.
    class FrameTimerGuard
    {
    public:
        explicit FrameTimerGuard(WaveformComponent& ownerIn)
            : owner(ownerIn), startMs(juce::Time::getMillisecondCounterHiRes()) {}

        ~FrameTimerGuard()
        {
            const double elapsed = juce::Time::getMillisecondCounterHiRes() - startMs;
            owner.lastFrameMs = elapsed;
            if (elapsed > owner.peakFrameMs.load())
                owner.peakFrameMs = elapsed;
        }

    private:
        WaveformComponent& owner;
        double startMs;

        JUCE_DECLARE_NON_COPYABLE(FrameTimerGuard)
    };

    // The sample range the currently-uploaded texture covers - so the
    // shader can map screen pixels to texels without needing the whole
    // file's worth of view-normalised coordinates.
    double textureViewStart = 0.0;
    double textureViewLength = 1.0;

    double viewStart = 0.0;   // in samples
    double viewLength = 44100.0 * 10.0; // in samples

    bool followPlayhead = false;

    // GL-thread-only: both the drawn line and (while following) the view
    // centring are computed from this single tracker, in renderOpenGL,
    // from the same reading - see the comment there.
    SmoothPositionTracker playheadLineTracker;

    double dragStartViewStart = 0.0;
    juce::Point<float> dragStartMouse;

    mutable juce::CriticalSection dataLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
};
