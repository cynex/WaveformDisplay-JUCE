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

    /** The GL rendering scale (e.g. 1.0, 1.25, 2.0 for a scaled/HiDPI display) - uploadWaveformTexture
        multiplies getWidth() by this to get the actual pixel width it targets, so anything computing
        the same "samples per pixel" quantity externally (e.g. diagnostics) needs it too, or it'll disagree
        with the real internal decision on scaled displays. */
    double getRenderingScale() const { return openGLContext.getRenderingScale(); }

    /** Wall-clock time the last GL draw call (glDrawArrays for the waveform quad) took, in milliseconds. */
    double getLastDrawCallMs() const { return lastDrawCallMs.load(); }
    /** Highest getLastDrawCallMs() has been since the app started. */
    double getPeakDrawCallMs() const { return peakDrawCallMs.load(); }

    /** Wall-clock time the last whole renderOpenGL() call took (texture rebuild when needed, plus the draw call), in milliseconds. */
    double getLastFrameMs() const { return lastFrameMs.load(); }
    /** Highest getLastFrameMs() has been since the app started. */
    double getPeakFrameMs() const { return peakFrameMs.load(); }

    // Diagnostics for tracking down the horizontal-jump bug: the raw,
    // unsnapped view start (in samples, from the playhead-tracker/follow
    // logic) and the pixel-snapped view start actually used for the
    // waveform's texture mapping (in screen pixels), both from the most
    // recent render. Also the single largest per-frame jump seen in the
    // SNAPPED view (in pixels) since this was last read - consumePeak...
    // resets it, so polling this at a UI timer rate will catch and hold a
    // spike even if the offending frame falls between polls.
    double getLastRawViewStartSamples() const { return diagRawViewStartSamples.load(); }
    double getLastSnappedViewStartPx() const { return diagSnappedViewStartPx.load(); }
    double consumePeakViewJumpPx()
    {
        return diagPeakViewJumpPx.exchange(0.0);
    }
    // Largest single-tick correction SmoothPositionTracker applied (seconds,
    // signed) since last read, and whether that correction was a full snap
    // (a jump treated as a real seek) rather than the smooth rate-based
    // catch-up - together these tell us whether the position TRACKER itself
    // is introducing a discontinuity, as opposed to something further down
    // the rendering pipeline.
    double consumePeakTrackerCorrectionSeconds() { return diagPeakTrackerCorrection.exchange(0.0); }
    int consumeSnapCount() { return diagSnapCount.exchange(0); }
    int consumeNotPlayingResetCount() { return diagNotPlayingResetCount.exchange(0); }
    // The ACTUAL raw-fallback-vs-mip decision uploadWaveformTexture made on
    // its last rebuild, and a counter of how many times that decision has
    // FLIPPED (raw->mip or mip->raw) since last read - set directly by
    // uploadWaveformTexture itself, so unlike any externally-recomputed
    // version of the same formula, this can't disagree with what actually
    // got rendered.
    bool getLastRawFallbackActive() const { return diagRawFallbackActive.load(); }
    int consumeRawFallbackFlipCount() { return diagRawFallbackFlipCount.exchange(0); }
    // How far the actual texture window's start sits from the current view
    // start, in screen pixels (from the last render) - and the single
    // largest percentage change in the texture window's LENGTH between
    // consecutive rebuilds since last read. The view-position diagnostics
    // above can read as perfectly smooth while the DISPLAYED content still
    // pops, if the texture window's length (and so its zoom/scale onto the
    // view) changes between rebuilds even though the view itself didn't -
    // this catches that case directly.
    double getLastTextureViewStartOffsetPx() const { return diagTextureViewStartOffsetPx.load(); }
    double consumePeakTextureScaleJumpPct() { return diagPeakTextureWindowJumpPx.exchange(0.0); }

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
    // (assuming ordinary 1x forward playback). An earlier version corrected
    // drift from ordinary audio-clock/system-clock mismatch by periodically
    // re-anchoring (fully or partially) straight to the raw reading - but
    // ANY discrete re-anchor, however small, is still a discontinuous jump
    // in the returned position, and the resulting VIEW shift (while
    // following) is inversely proportional to samples-per-pixel: a fixed,
    // tiny real-world correction stays sub-pixel and invisible at a shallow
    // zoom, but becomes a visible multi-pixel jump once zoomed in enough -
    // exactly what kept reappearing regardless of which render path
    // (mip-pyramid or raw-sample fallback) was in use, since both read
    // their view position from this same tracker. Instead, ordinary drift
    // is now corrected by temporarily running the extrapolation at a
    // slightly different RATE until it catches up - the returned position
    // itself never jumps at all, it just runs a little faster or slower for
    // a moment. Only a genuinely large discrepancy (a real seek/loop) snaps
    // immediately, since there's no "drift" to smooth there.
    class SmoothPositionTracker
    {
    public:
        // outCorrectionSeconds (if non-null) receives the total gap between
        // what plain dead-reckoning from the PREVIOUS call's trajectory
        // would have given this instant, and what's actually returned - a
        // non-trivial value here means THIS function introduced a
        // discontinuity (whether from the snap branch or the trailing
        // ahead-of-raw clamp), as opposed to a jump coming from further
        // down the rendering pipeline. outWasSnap/outWasNotPlayingReset
        // report which path produced it.
        double update(double rawPositionSeconds, bool isPlaying, double* outCorrectionSeconds = nullptr, bool* outWasSnap = nullptr, bool* outWasNotPlayingReset = nullptr)
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();

            if (outCorrectionSeconds != nullptr) *outCorrectionSeconds = 0.0;
            if (outWasSnap != nullptr) *outWasSnap = false;
            if (outWasNotPlayingReset != nullptr) *outWasNotPlayingReset = false;

            if (!isPlaying)
            {
                if (valid && outWasNotPlayingReset != nullptr)
                    *outWasNotPlayingReset = true;
                valid = false;
                return rawPositionSeconds;
            }

            if (!valid)
            {
                anchorPosition = rawPositionSeconds;
                anchorTimeMs = nowMs;
                rate = 1.0;
                lastRawSeen = rawPositionSeconds;
                valid = true;
                return rawPositionSeconds;
            }

            // What continuing the PREVIOUS trajectory unmodified would have
            // given right now - the baseline outCorrectionSeconds measures
            // the final return value against.
            const double deadReckoned = anchorPosition + (nowMs - anchorTimeMs) / 1000.0 * rate;
            double estimated = deadReckoned;

            if (rawPositionSeconds != lastRawSeen)
            {
                const double error = rawPositionSeconds - estimated;
                if (std::abs(error) > 0.2)
                {
                    // Big enough to be a real seek/loop - snap immediately,
                    // there's no drift to smooth here.
                    anchorPosition = rawPositionSeconds;
                    anchorTimeMs = nowMs;
                    rate = 1.0;
                    estimated = rawPositionSeconds;
                    if (outWasSnap != nullptr) *outWasSnap = true;
                }
                else
                {
                    // Re-anchor from the CURRENT estimate (not the raw
                    // value) so the returned position stays perfectly
                    // continuous right here, then let a mildly adjusted
                    // rate close the remaining gap smoothly over roughly
                    // the next third of a second. Clamped to +/-50% speed
                    // as a generous safety bound - genuine clock drift is
                    // orders of magnitude smaller than that, so in practice
                    // this correction is imperceptible.
                    anchorPosition = estimated;
                    anchorTimeMs = nowMs;
                    rate = 1.0 + juce::jlimit(-0.5, 0.5, error / 0.3);
                }
                lastRawSeen = rawPositionSeconds;
            }

            // Don't let timing assumptions run us more than one buffer
            // period ahead of the last actual reading.
            estimated = juce::jmin(estimated, rawPositionSeconds + 0.25);

            if (outCorrectionSeconds != nullptr)
                *outCorrectionSeconds = estimated - deadReckoned;

            return estimated;
        }

    private:
        double lastRawSeen = -1.0;
        double anchorPosition = 0.0;
        double anchorTimeMs = 0.0;
        double rate = 1.0;
        bool valid = false;
    };

    AudioEngine& audioEngine;
    juce::OpenGLContext openGLContext;
    WaveformParameters params;

    // Diagnostics (see the getters above) - written on the GL thread every
    // frame, read from the message thread.
    std::atomic<double> diagRawViewStartSamples { 0.0 };
    std::atomic<double> diagSnappedViewStartPx { 0.0 };
    std::atomic<double> diagPeakViewJumpPx { 0.0 };
    double diagPrevSnappedViewStartPx = 0.0; // GL-thread-only
    bool diagPrevValid = false;              // GL-thread-only
    std::atomic<double> diagPeakTrackerCorrection { 0.0 };
    std::atomic<int> diagSnapCount { 0 };
    std::atomic<int> diagNotPlayingResetCount { 0 };
    std::atomic<bool> diagRawFallbackActive { false };
    std::atomic<int> diagRawFallbackFlipCount { 0 };
    bool diagPrevRawFallbackActive = false; // GL-thread-only
    bool diagPrevRawFallbackValid = false;   // GL-thread-only
    std::atomic<double> diagTextureViewStartOffsetPx { 0.0 };
    std::atomic<double> diagPeakTextureWindowJumpPx { 0.0 };
    double diagPrevTextureViewLength = 0.0; // GL-thread-only
    bool diagPrevTextureViewStartValid = false; // GL-thread-only

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
