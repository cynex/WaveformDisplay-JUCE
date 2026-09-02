#pragma once

#include <atomic>
#include <cmath>
#include <list>
#include <unordered_map>
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
    Click + drag -> turntable-style scratches the audio if playback is
                    already running when the click lands (AudioEngine's
                    ScratchSource continuously chases a target position that
                    moves RELATIVE to mouse movement, inverted - dragging
                    right moves it backwards, like pulling a record towards
                    you - rather than jumping to an absolute cursor
                    position), otherwise pans the view. Decided once per
                    gesture at mouseDown so starting/stopping playback
                    mid-drag can't change its meaning underneath the user.
                    A plain click while paused (no drag) seeks once, on
                    release.

    Texture rebuilds run on a dedicated background thread (see run() /
    buildTextureData()) rather than inline in renderOpenGL(): the GL thread
    just posts what view it needs, keeps drawing with whatever texture is
    already active, and picks up the finished result (a plain data-only GL
    upload) whenever it's ready. This trades a small amount of lag - the
    visible texture can trail the view target by a frame or more under heavy
    load, still correctly mapped via textureViewStart/textureViewLength, just
    slightly stale/coarser until the worker catches up - for never stalling
    a frame outright on the CPU packing work, which is the more noticeable
    problem (a dropped/late frame) of the two.
*/
class WaveformComponent : public juce::Component,
                           public juce::OpenGLRenderer,
                           private juce::Timer,
                           private juce::Thread
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
    void notifyFileChanged();

    /** When enabled, the view continuously recentres on the playhead during playback. */
    void setFollowPlayhead(bool shouldFollow) { followPlayhead = shouldFollow; }
    bool getFollowPlayhead() const { return followPlayhead; }

    /** Call after changing AudioEngine's playback position from outside this
        class (e.g. a rewind button) while paused. Continuous repainting is
        off whenever nothing's playing, so a position change made without
        going through this component's own mouse handling would otherwise
        sit unseen until something else asked for a redraw. */
    void notifyPositionChangedExternally() { openGLContext.triggerRepaint(); }

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
    void mouseUp(const juce::MouseEvent&) override;

    // juce::OpenGLRenderer
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

private:
    void timerCallback() override;
    void buildShaders();
    void clampView();
    void seekToScreenX(float screenX);

    // --- Tile cache -----------------------------------------------------
    // The visible-range texture built each rebuild is assembled from fixed-
    // size "tiles" of blocks (kTileBlocks blocks wide, at whatever mip level
    // is chosen) rather than re-packed from scratch every time. Panning back
    // over a region that's already been visited (extremely common - back
    // and forth scrubbing, zooming back out then back in near the same
    // spot) reuses the cached tile's already-packed bytes via a straight
    // memcpy instead of re-running the packing loop, which is the
    // "chunk the waveform into image tiles" ask. Tiles are cheap (a handful
    // of KB each) so an LRU cap of kMaxCachedTiles just bounds memory for
    // very long scrubbing sessions rather than acting as a real constraint.
    //
    // Owned EXCLUSIVELY by the background build thread (run()/
    // buildTextureData()/getOrBuildTile()) - never touched from the GL or
    // message thread - so none of it needs its own lock.
    struct TileData
    {
        std::vector<juce::uint8> waveform; // 4 bytes/block: R=min,G=max,B=lowE,A=highE
        std::vector<juce::uint8> mid;      // 4 bytes/block: R=midE (G/B/A unused)
        int numBlocks = 0;
    };

    static constexpr int kTileBlocks = 1024;
    static constexpr size_t kMaxCachedTiles = 1024;

    static juce::uint64 tileKey(int level, int tileIndex)
    {
        return (static_cast<juce::uint64>(static_cast<juce::uint32>(level)) << 32)
             | static_cast<juce::uint32>(tileIndex);
    }

    const TileData& getOrBuildTile(int level, int tileIndex, const std::vector<WaveformBlock>& levelBlocks);
    void clearTileCache();

    std::unordered_map<juce::uint64, TileData> tileCache;
    std::list<juce::uint64> tileLru;
    std::unordered_map<juce::uint64, std::list<juce::uint64>::iterator> tileLruPos;
    // ---------------------------------------------------------------------

    // --- Background texture build thread ---------------------------------
    // What one rebuild needs to know, and what it produces - see the class
    // comment for the overall design. Requests are a single-slot mailbox
    // (a newer post just overwrites the pending one) rather than a queue,
    // so the worker always works towards the MOST RECENT view target
    // instead of laboriously catching up through every intermediate one
    // requested during a fast pan/zoom.

    // Requested texture window is this many times wider (in blocks) than
    // the screen actually needs, so the active texture keeps covering the
    // view for longer while zooming/scrolling during playback outruns the
    // background thread - the more it lags, the more of the screen would
    // otherwise fall outside the texture's coverage and show as a
    // pixelated/stretched edge (clamped to whichever real block sits at the
    // texture's boundary - see the shader's outOfRangeCoverage). This
    // doesn't reduce resolution (level selection still targets one texel
    // per screen pixel; a wider request can only push it to a FINER level
    // if one now fits), it just costs a bit more memory/CPU per rebuild in
    // exchange for needing fewer of them to keep up.
    static constexpr float kTextureWindowPadding = 2.0f;
    struct BuildRequest
    {
        double viewStart = 0.0;
        double viewLength = 1.0;
        int targetWidth = 1;
        int safeMaxWidth = 4096;
        juce::uint32 generation = 0;
    };

    struct BuildResult
    {
        juce::uint32 generation = 0;
        int textureWidth = 0;
        std::vector<juce::uint8> pixels;
        std::vector<juce::uint8> midPixels;
        double textureViewStart = 0.0;
        double textureViewLength = 1.0;
    };

    void run() override; // juce::Thread
    BuildResult buildTextureData(const BuildRequest& request);
    void applyBuildResult(BuildResult& result); // GL-thread-only

    juce::WaitableEvent buildRequestEvent;

    juce::CriticalSection requestLock;
    BuildRequest pendingRequest;
    bool hasPendingRequest = false;

    juce::CriticalSection resultLock;
    BuildResult readyResult;
    bool hasReadyResult = false;

    // Bumped (message/GL thread, wherever a rebuild is requested from) for
    // every posted request and compared against a completed result's own
    // generation so the GL thread can tell "a fresher result than what's
    // currently on screen" apart from "the same one we already applied".
    std::atomic<juce::uint32> nextGeneration { 1 };
    juce::uint32 displayedGeneration = 0; // GL-thread-only

    // GL_MAX_TEXTURE_SIZE can only be queried with a current GL context, so
    // it's read once in newOpenGLContextCreated (GL thread) and cached here
    // for the background thread (which has no GL context at all) to read.
    std::atomic<int> safeMaxTextureWidth { 4096 };

    // Worker-thread-only: which AudioEngine::getLoadGeneration() the tile
    // cache above was built against, so a new file (detected via that
    // counter changing) drops the old file's now-meaningless cached tiles
    // without the message thread having to reach across and clear it itself.
    int workerCachedLoadGeneration = -1;
    // ---------------------------------------------------------------------

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

    // Turns a raw, block-granular amplitude reading (which jumps discretely
    // as the playhead crosses each analysis block boundary) into a smooth
    // visual "pulse": fast attack so a loud hit reads as an immediate
    // punch, slower release so it decays back down over a beat or so
    // instead of vanishing instantly - the shape that actually reads as
    // "pulsing to the beat" rather than a flickering step function. Forced
    // towards 0 while not playing (see the isPlaying parameter),
    // rather than freezing on its last value, so the pulse settles back
    // down after playback stops instead of leaving the waveform stuck
    // mid-bulge.
    class AmplitudePulseTracker
    {
    public:
        float update(float rawEnergy01, bool isPlaying)
        {
            const double nowMs = juce::Time::getMillisecondCounterHiRes();
            const double dtSeconds = validLastTime ? juce::jlimit(0.0, 0.25, (nowMs - lastTimeMs) / 1000.0) : 0.0;
            lastTimeMs = nowMs;
            validLastTime = true;

            const float target = isPlaying ? juce::jlimit(0.0f, 1.0f, rawEnergy01) : 0.0f;
            const double tau = target > smoothed ? attackTauSeconds : releaseTauSeconds;
            const float coeff = dtSeconds > 0.0 ? (float) (1.0 - std::exp(-dtSeconds / tau)) : 0.0f;

            smoothed += (target - smoothed) * coeff;
            return smoothed;
        }

    private:
        static constexpr double attackTauSeconds  = 0.03; // fast rise - reads as an immediate punch on a hit
        static constexpr double releaseTauSeconds = 0.25; // slower fall - gives the pulse its visible decay/bounce

        float smoothed = 0.0f;
        double lastTimeMs = 0.0;
        bool validLastTime = false;
    };

    // GL-thread-only (called once per renderOpenGL, alongside the other
    // playhead reads there). Reads the mip pyramid directly rather than via
    // the display texture - the texture only covers whatever window is
    // currently uploaded and lags the background build thread; the
    // amplitude pulse needs to react to what's happening AT THE PLAYHEAD
    // right now, which the source analysis data always has regardless of
    // what's on screen. Returns a blend of the block's low/mid/high band
    // energies weighted by computeAmplitudeBandWeights(), matching the
    // shader's own frequency-focused amplitude driver, rather than the raw
    // broadband peak. Takes the max over a small window of blocks around
    // the target sample (rather than a single block) so the reading isn't
    // fully at the mercy of exactly which block boundary the playhead
    // happens to be crossing at this instant - AmplitudePulseTracker's own
    // attack/release filtering handles the remaining smoothing.
    float sampleAmplitudeAtSample(double sampleIndex) const;

    // GL-thread-only, same calling convention as sampleAmplitudeAtSample
    // above, but returns a single band's energy DIRECTLY (no weighting) -
    // lowEnergy if highBand is false, highEnergy if true - for the two
    // independent low/high colour-glow pulses (AmplitudePulseTracker
    // instances amplitudeLowPulseTracker/amplitudeHighPulseTracker), which
    // are deliberately NOT tied to amplitudeMinFrequencyHz/
    // amplitudeMaxFrequencyHz (that range only shapes the HEIGHT boost) -
    // each colour always tracks its own named band specifically.
    float sampleBandEnergyAtSample(double sampleIndex, bool highBand) const;

    // Cheap approximation of "amplitude in a chosen Hz range" from the
    // THREE fixed bands AudioEngine already analyses (split at 300Hz and
    // 3000Hz - see AudioEngine::analyse) rather than a true band-pass
    // reanalysis: returns how much of [minHz, maxHz] overlaps each of the
    // low/mid/high bands, normalised to sum to 1. Used identically by the
    // GL thread (to set the shader's amplitudeBandWeights uniform) and by
    // sampleAmplitudeAtSample (to blend a block's low/mid/highEnergy the
    // same way), so the CPU-driven pulse and the shader's own per-block
    // boost/glow are always focused on the same range. A degenerate range
    // (minHz >= maxHz after clamping) falls back to an even 1/3 split
    // across all three bands rather than dividing by zero.
    static void computeAmplitudeBandWeights(float minHz, float maxHz, float& lowWeight, float& midWeight, float& highWeight);

    AudioEngine& audioEngine;
    juce::OpenGLContext openGLContext;
    WaveformParameters params;

    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    GLuint vertexBuffer = 0;

    // Double-buffered ("vblank buffered") texture pairs: index [0] and [1]
    // each hold a complete waveform+mid texture. A rebuild always writes
    // into the INACTIVE slot (the one not currently bound for sampling by
    // the draw call), and activeTextureSlot only flips to it once the
    // upload has fully completed - so the texture actively being sampled
    // this frame is never the one mid-mutation, and a slow rebuild can never
    // leave the screen showing a half-updated texture. This mirrors classic
    // front/back-buffer double buffering, just for this one texture rather
    // than the whole framebuffer (which JUCE's OpenGLContext already
    // double-buffers via the driver/vsync swap chain).
    GLuint waveformTextures[2] = { 0, 0 };   // R=min, G=max, B=lowEnergy, A=highEnergy
    GLuint midTextures[2] = { 0, 0 };        // R=midEnergy (G/B/A unused)
    int allocatedTextureWidths[2] = { -1, -1 }; // GL-thread-only: what glTexImage2D last allocated per slot
    int activeTextureSlot = 0;

    // Flipped true from the message thread (setViewRange, resized) and read
    // and cleared from the GL render thread (renderOpenGL, once it's posted
    // a rebuild request for the current view to the background thread) -
    // atomic so that handoff is properly visible across threads.
    std::atomic<bool> textureDirty { true };
    int textureWidth = 0;

    // Message-thread-only: mirrors whatever openGLContext.setContinuousRepainting()
    // was last actually called with, so timerCallback() only calls it again
    // (a real, if small, cost) on an actual change instead of every tick.
    bool continuousRepaintActive = false;

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

    // GL-thread-only: drives the shader's amplitudePulse uniform (the
    // HEIGHT boost only) - see AmplitudePulseTracker's own comment.
    AmplitudePulseTracker amplitudePulseTracker;

    // GL-thread-only: drive the shader's amplitudeLowPulse/amplitudeHighPulse
    // uniforms respectively - independent of amplitudePulseTracker above and
    // of each other, so the low and high colour glows can each punch on
    // their own band's own timing rather than sharing one reading.
    AmplitudePulseTracker amplitudeLowPulseTracker;
    AmplitudePulseTracker amplitudeHighPulseTracker;

    double dragStartViewStart = 0.0;
    juce::Point<float> dragStartMouse;
    bool draggedPastClickThreshold = false;
    bool scrubbingThisDrag = false;
    double scratchTargetSeconds = 0.0; // running target while scratchingThisDrag, updated relatively each mouseDrag

    mutable juce::CriticalSection dataLock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WaveformComponent)
};
