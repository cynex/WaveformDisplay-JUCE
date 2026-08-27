#include "WaveformComponent.h"

using namespace juce::gl;

namespace
{
    // Full-screen quad in clip space, covering the whole component.
    constexpr float quadVerts[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f
    };

    const char* vertexShaderSrc = R"(
        attribute vec2 position;
        varying vec2 vUv;
        void main()
        {
            vUv = position * 0.5 + 0.5;
            gl_Position = vec4(position, 0.0, 1.0);
        }
    )";

    // The waveform texture is Nx1, RGBA8, and is rebuilt on the CPU (see
    // WaveformComponent::uploadWaveformTexture) from whichever mip level's
    // FIXED buckets currently fit under the texture size limit, at close to
    // one texel per screen pixel - each texel already holds the EXHAUSTIVE
    // min/max/energy over every underlying analysis block it covers, not a
    // sparse sample of them (an earlier version did a bounded-tap reduction
    // in this shader instead; capping the tap count meant it could skip
    // peaks, and which peaks got skipped changed as the view moved by
    // sub-pixel amounts, which is what caused the whole waveform to visibly
    // flicker/flutter).
    //
    // Because the texture's buckets are snapped to whole fixed blocks, the
    // sample range it covers is very slightly wider than the exact visible
    // window - texMapOffset/texMapScale correct for that so screen x still
    // maps precisely onto the true view, instead of onto the texture's
    // (very slightly rounded, and differently rounded frame to frame while
    // panning) span. Skipping that correction is what made panning judder.
    //
    // GL_NEAREST means every screen pixel's texture lookup commits fully to
    // whichever single block is closest, with a hard, un-anti-aliased flip
    // the instant the pixel's mapped position crosses the boundary to the
    // next block. With no boundary anti-aliasing, a pixel that happens to
    // sit almost exactly on such a boundary flips back and forth between
    // the two blocks' (often quite different) heights on essentially any
    // sub-pixel wobble - visible as flicker at block edges, independent of
    // frame rate since it isn't a pacing problem. The fix looks up BOTH
    // neighbouring blocks explicitly and cross-fades the resulting SDF
    // COVERAGE between them over a narrow band - i.e. proper edge
    // anti-aliasing of the shape - rather than blending the min/max VALUES
    // (which is what the peak "breathing" bug earlier was, and would bring
    // it back).
    //   waveformTex: R = minValue (-1..1), G = maxValue (-1..1), B = lowEnergy (0..1), A = highEnergy (0..1)
    //   midTex:      R = midEnergy (0..1) - a second texture since RGBA8 was already full
    const char* fragmentShaderSrc = R"(
        varying vec2 vUv;

        uniform sampler2D waveformTex;
        uniform sampler2D midTex;       // R = midEnergy (0..1); G/B/A unused
        uniform float texWidth;        // texels
        uniform float texMapOffset;    // (viewStart - textureViewStart) / textureViewLength
        uniform float texMapScale;     // viewLength / textureViewLength
        uniform float aaWidth;         // pixels
        uniform float smoothing;       // extra feather, 0..1
        uniform float pixelHeight;     // component height in pixels
        uniform float pixelWidth;      // component width in pixels
        uniform vec3 solidColour;
        uniform vec3 lowColour;
        uniform vec3 midColour;
        uniform vec3 highColour;
        uniform float lowAmount;
        uniform float midAmount;
        uniform float highAmount;
        uniform float tintEnabled;      // 0 or 1 - master switch for low/mid/high tinting
        uniform vec3 backgroundColour;
        uniform vec3 playheadColour;
        uniform float playheadViewFrac; // playback position, 0..1 across the CURRENT view
        uniform float playheadVisible;  // 0 or 1
        uniform float centreLineAlpha;  // opacity of the solid white zero-amplitude axis line, 0..1

        float coverageFor(vec4 texel, float y, float edge, float smoothingAmount)
        {
            float minV = texel.r * 2.0 - 1.0;
            float maxV = texel.g * 2.0 - 1.0;

            // Guard against a degenerate (silent) block so it still renders a thin line.
            float halfHeight = max((maxV - minV) * 0.5, 0.002);
            float centre = (maxV + minV) * 0.5;

            // The extra smoothing feather is scaled by this block's OWN
            // half-height rather than being a fixed screen-space amount.
            // Real audio blocks vary hugely in amplitude - a quiet block
            // sitting next to a loud transient - and a fixed feather can be
            // many times a quiet block's actual height, making it look
            // disproportionately hazy relative to its neighbours. As blocks
            // with very different amplitudes scroll past, that
            // disproportionate haze changing block to block is what read as
            // flicker that got worse the higher smoothing was pushed.
            float feather = edge + smoothingAmount * halfHeight * 2.0;

            // Signed distance (in amplitude units) from the fragment to the envelope band:
            // negative = inside the waveform, positive = outside.
            float dist = abs(y - centre) - halfHeight;
            return 1.0 - smoothstep(-feather, feather, dist);
        }

        void main()
        {
            float texU = texMapOffset + vUv.x * texMapScale;

            // Texel-centre convention: texel i's centre sits at (i+0.5)/texWidth,
            // so this recovers a continuous texel index whose fractional part
            // is exactly how far the fragment sits from idx0's centre towards idx1's.
            float texIndexF = texU * texWidth - 0.5;
            float idx0 = floor(texIndexF);
            float frac = texIndexF - idx0;
            float idx1 = idx0 + 1.0;
            idx0 = clamp(idx0, 0.0, texWidth - 1.0);
            idx1 = clamp(idx1, 0.0, texWidth - 1.0);

            vec4 texel0 = texture2D(waveformTex, vec2((idx0 + 0.5) / texWidth, 0.5));
            vec4 texel1 = texture2D(waveformTex, vec2((idx1 + 0.5) / texWidth, 0.5));
            float midE0 = texture2D(midTex, vec2((idx0 + 0.5) / texWidth, 0.5)).r;
            float midE1 = texture2D(midTex, vec2((idx1 + 0.5) / texWidth, 0.5)).r;

            // Fragment's y position in the same -1..1 amplitude space as the envelope.
            float y = (vUv.y * 2.0 - 1.0);

            // Convert the AA width parameter (pixel-space) into the same
            // normalised amplitude units used above. Smoothing is applied
            // per-block, scaled by each block's own height (see coverageFor).
            float pixelToAmp = 2.0 / max(pixelHeight, 1.0);
            float edge = max(aaWidth * pixelToAmp, 0.0001);

            float coverage0 = coverageFor(texel0, y, edge, smoothing);
            float coverage1 = coverageFor(texel1, y, edge, smoothing);

            // Blend width for the boundary crossfade, in the same fractional
            // texel units as `frac`. This is fundamental anti-aliasing of the
            // hard block-to-block edge, not a stylistic effect - it should
            // stay about half a SCREEN PIXEL wide (converted into texel
            // units via texelsPerPixel) and must NOT scale with aaWidth: at
            // the default aaWidth of 1.5 that would make the half-width
            // 0.75 texels, i.e. a "boundary" blend wider than the gap
            // between texel centres - meaning almost every pixel was
            // constantly cross-fading between two different blocks' heights
            // instead of committing to the nearest one, which is exactly
            // the peak "breathing" bug back again, just self-inflicted this
            // time instead of coming from GL_LINEAR - and, unlike the
            // GL_LINEAR case, present regardless of what's moving the view,
            // which is why it started showing up during panning too.
            float texelsPerPixel = texWidth / max(pixelWidth, 1.0);
            float halfBlend = max(texelsPerPixel * 0.5, 0.02);
            float blend = smoothstep(0.5 - halfBlend, 0.5 + halfBlend, frac);

            float coverage = mix(coverage0, coverage1, blend);
            float lowE = mix(texel0.b, texel1.b, blend);
            float midE = mix(midE0, midE1, blend);
            float highE = mix(texel0.a, texel1.a, blend);

            // A true three-way interpolation between the low/mid/high
            // colours, weighted by each band's relative energy (rather than
            // layering three independent multiplicative tints on top of
            // solidColour) - the previous approach left the plain white
            // base visibly showing through whenever a block wasn't strongly
            // dominant in all three bands at once, which was most blocks
            // even with every amount at 1.0.
            float wLow = max(lowE * lowAmount, 0.0);
            float wMid = max(midE * midAmount, 0.0);
            float wHigh = max(highE * highAmount, 0.0);
            float wSum = wLow + wMid + wHigh;
            vec3 freqBlend = wSum > 0.0001 ? (lowColour * wLow + midColour * wMid + highColour * wHigh) / wSum
                                            : vec3(1.0);

            vec3 tint = solidColour * mix(vec3(1.0), freqBlend, tintEnabled);

            vec3 outColour = mix(backgroundColour, tint, coverage);

            // Solid white 1px line through the zero-amplitude axis (drawn
            // under the playhead line, and under the waveform - it should
            // read as sitting behind/through the waveform, not painted over
            // it), opacity controlled by centreLineAlpha.
            if (centreLineAlpha > 0.0)
            {
                float pixelToNormY = 1.0 / max(pixelHeight, 1.0);
                float centreLineHalfWidth = 1.0 * pixelToNormY;
                float dyCentre = abs(vUv.y - 0.5);
                float centreLineCoverage = 1.0 - smoothstep(centreLineHalfWidth * 0.5, centreLineHalfWidth * 1.5, dyCentre);
                outColour = mix(outColour, vec3(1.0), centreLineCoverage * centreLineAlpha);
            }

            // Playhead: a thin anti-aliased vertical line at the current playback position.
            if (playheadVisible > 0.5)
            {
                // Deliberately independent of aaWidth - the playhead should
                // always read as a crisp 1px line regardless of how soft the
                // waveform's own edges are set to.
                float pixelToNormX = 1.0 / max(pixelWidth, 1.0);
                float lineHalfWidth = 1.0 * pixelToNormX;
                float dx = abs(vUv.x - playheadViewFrac);
                float lineCoverage = 1.0 - smoothstep(lineHalfWidth * 0.5, lineHalfWidth * 1.5, dx);
                outColour = mix(outColour, playheadColour, lineCoverage);
            }

            gl_FragColor = vec4(outColour, 1.0);
        }
    )";
}

WaveformComponent::WaveformComponent(AudioEngine& engine) : audioEngine(engine)
{
    openGLContext.setRenderer(this);
    openGLContext.attachTo(*this);
    // Continuous repainting drives the GL thread's own render loop (paced by
    // the driver/vsync) instead of an independent CPU Timer calling
    // triggerRepaint(). A Timer's tick rate/jitter doesn't line up with the
    // display's actual refresh, which is its own source of jutter on top of
    // anything content-related - every render now also reads the playhead
    // position directly from AudioEngine, fresh, so motion stays smooth
    // regardless of how the GL thread's cadence relates to the
    // message-thread timer below.
    openGLContext.setContinuousRepainting(true);
    // JUCE defaults this to 1 (vsync on) already; set it explicitly so
    // continuous repainting is definitely paced by the display refresh and
    // not free-running (which can tear/look inconsistent) on whatever
    // platform/driver combination this ends up running on.
    openGLContext.setSwapInterval(1);
    // This timer no longer triggers repaints itself - it just periodically
    // recentres the view during follow-playhead, which has to happen on the
    // message thread (setViewRange touches the scrollbar Component).
    startTimerHz(60);
}

WaveformComponent::~WaveformComponent()
{
    openGLContext.detach();
}

void WaveformComponent::setParameters(const WaveformParameters& newParams)
{
    params = newParams;
    openGLContext.triggerRepaint();
}

void WaveformComponent::setViewRange(double newViewStart, double newViewLength)
{
    // setViewRange is called from the message thread (mouse handling, and
    // the follow-playhead timer), while renderOpenGL/uploadWaveformTexture
    // read viewStart/viewLength from JUCE's separate GL render thread with
    // no other synchronisation. Without this lock, the GL thread could read
    // a viewStart written by one call here together with a viewLength still
    // pending from the next, or build the texture from one snapshot and
    // then compute the screen mapping from another - each is a one-frame
    // misalignment, invisible from a single sporadic call (manual panning)
    // but happening continuously while follow-playhead calls this every
    // tick, which is what read as constant jutter only in that mode.
    const juce::ScopedLock sl(dataLock);
    viewStart = newViewStart;
    viewLength = newViewLength;
    clampView();
    textureDirty = true;
    if (onViewRangeChanged != nullptr)
        onViewRangeChanged();
    openGLContext.triggerRepaint();
}

double WaveformComponent::getTotalLength() const
{
    return (double) audioEngine.getTotalNumSamples();
}

void WaveformComponent::clampView()
{
    const double total = getTotalLength();
    if (total <= 0.0)
        return;

    viewLength = juce::jlimit(64.0, total, viewLength);
    viewStart = juce::jlimit(0.0, total - viewLength, viewStart);
}

void WaveformComponent::resized()
{
    // The texture is now sized to roughly match the component's pixel
    // width (see uploadWaveformTexture), so a resize needs a rebuild too.
    textureDirty = true;
}

void WaveformComponent::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const double total = getTotalLength();
    if (total <= 0.0)
        return;

    // Sample under the cursor, used as the fixed point for zooming.
    const double relX = juce::jlimit(0.0, 1.0, (double) e.position.x / juce::jmax(1, getWidth()));
    const double sampleUnderCursor = viewStart + relX * viewLength;

    const double zoomFactor = std::pow(0.85, wheel.deltaY * 10.0 * (wheel.isReversed ? -1.0 : 1.0));
    double newLength = juce::jlimit(64.0, total, viewLength * zoomFactor);

    double newStart = sampleUnderCursor - relX * newLength;

    setViewRange(newStart, newLength);
}

void WaveformComponent::seekToScreenX(float screenX)
{
    const double total = getTotalLength();
    const double sampleRate = audioEngine.getSampleRate();
    if (total > 0.0 && sampleRate > 0.0 && getWidth() > 0)
    {
        const double relX = juce::jlimit(0.0, 1.0, (double) screenX / (double) getWidth());
        const double sampleUnderCursor = juce::jlimit(0.0, total, viewStart + relX * viewLength);
        audioEngine.setPosition(sampleUnderCursor / sampleRate);
    }
}

void WaveformComponent::mouseDown(const juce::MouseEvent& e)
{
    dragStartViewStart = viewStart;
    dragStartMouse = e.position;
    draggedPastClickThreshold = false;
}

void WaveformComponent::mouseDrag(const juce::MouseEvent& e)
{
    const double total = getTotalLength();
    if (total <= 0.0 || getWidth() <= 0)
        return;

    const float dx = e.position.x - dragStartMouse.x;

    // Only committing to a pan once the drag has moved a few pixels (rather
    // than on every mouseDrag, which fires even for a near-zero-movement
    // click) keeps a plain click from being treated as a pan - see mouseUp,
    // which is what actually seeks the playhead for a genuine click. Without
    // this distinction, seeking on mouseDown moved the playhead (and, while
    // follow-playhead is on and playing, immediately recentred the view)
    // before the drag's own pan had a chance to take over, reading as a
    // spurious jump/flutter at the start of every click-drag.
    if (!draggedPastClickThreshold && std::abs(dx) < 3.0f)
        return;

    draggedPastClickThreshold = true;

    const double samplesPerPixel = viewLength / (double) getWidth();
    setViewRange(dragStartViewStart - dx * samplesPerPixel, viewLength);
}

void WaveformComponent::mouseUp(const juce::MouseEvent& e)
{
    // A genuine click (not a pan) seeks the playhead to wherever the cursor
    // landed - deferred to mouseUp rather than mouseDown so a click-drag pan
    // never has its start point misread as a seek (see mouseDrag).
    if (!draggedPastClickThreshold)
        seekToScreenX(e.position.x);
}

void WaveformComponent::timerCallback()
{
    // The view itself is now recentred directly from renderOpenGL (GL
    // thread) every frame while following, using the exact same playhead
    // reading the line is drawn from - see the comment there for why. This
    // timer's only remaining job is refreshing the scrollbar's Component
    // state to match, which (unlike the view's own sample-range doubles)
    // has to happen on the message thread - a periodic refresh is plenty
    // for a position indicator, so this doesn't need to run at render rate.
    if (followPlayhead && onViewRangeChanged != nullptr)
        onViewRangeChanged();
}

void WaveformComponent::newOpenGLContextCreated()
{
    buildShaders();

    glGenBuffers(1, &vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);

    glGenTextures(1, &waveformTexture);
    glBindTexture(GL_TEXTURE_2D, waveformTexture);
    // NOTE: deliberately GL_RGBA/GL_UNSIGNED_BYTE, not a float format. A
    // GL_RGBA32F texture is not a valid/complete combination under the
    // GLES2-style context JUCE gives us here (attribute/varying/gl_FragColor
    // shaders) - sampling an incomplete texture returns (0,0,0,1) for every
    // texel, which read back as a flat line at the centre. Plain 8-bit RGBA
    // is universally supported.
    //
    // GL_NEAREST, not GL_LINEAR, is deliberate: each texel's min/max is a
    // real, physically meaningful peak for the sample range it covers -
    // smoothly interpolating that VALUE toward a neighbouring block's is
    // not (there's no such thing as "70% of the way between these two
    // peaks" in the source audio). Doing that made a single tall, one-block
    // peak visibly shrink and grow ("breathe") as the sub-pixel alignment
    // between it and the screen changed - imperceptible during chunky
    // mouse-driven panning, but very visible as continuous jutter right at
    // peaks while smoothly auto-scrolling during playhead-follow. Each
    // block is rendered as a sharp-edged bar instead; visual softness comes
    // from the SDF's own aaWidth/smoothing params (which anti-alias the
    // bar's EDGES, not its height) rather than from texture filtering.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Second texture for mid-band energy - waveformTexture's RGBA8 channels
    // were already fully spoken for (min/max/lowEnergy/highEnergy).
    glGenTextures(1, &midTexture);
    glBindTexture(GL_TEXTURE_2D, midTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    allocatedTextureWidth = -1;
    textureDirty = true;
}

void WaveformComponent::buildShaders()
{
    auto newShader = std::make_unique<juce::OpenGLShaderProgram>(openGLContext);

    if (newShader->addVertexShader(vertexShaderSrc)
        && newShader->addFragmentShader(fragmentShaderSrc)
        && newShader->link())
    {
        shader = std::move(newShader);
    }
    else
    {
        DBG(newShader->getLastError());
        jassertfalse;
    }
}

void WaveformComponent::uploadWaveformTexture(double viewStartSamples, double viewLengthSamples)
{
    // A texture width equal to the raw (finest) analysis block count (which
    // can be 100,000+ for a full song) silently exceeds GL_MAX_TEXTURE_SIZE
    // on every GPU (usually 8192-16384) - glTexImage2D then fails and the
    // texture is left uninitialised.
    GLint maxTexSize = 4096;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
    const int safeMaxWidth = juce::jmin((int) maxTexSize, 8192);

    // Target roughly one texel per screen pixel. This isn't just an
    // optimisation: the fragment shader does a single texture lookup per
    // pixel with no further reduction, so every texel must already be the
    // EXHAUSTIVE min/max/energy over whatever it covers (see the mip-level
    // choice below) - otherwise pixels could still skip peaks and flicker
    // as the view moves by sub-pixel amounts.
    const int pixelWidth = juce::jmax(1, (int) std::ceil((double) getWidth() * openGLContext.getRenderingScale()));
    const int targetWidth = juce::jmin(pixelWidth, safeMaxWidth);

    const double baseSamplesPerBlock = juce::jmax(1, audioEngine.getNumSamplesPerBlock());
    const int numLevels = audioEngine.getNumMipLevels();

    // Pick the finest mip level whose fixed buckets still fit the visible
    // range inside the target texture width. Using the pyramid's fixed,
    // sample-anchored bucket boundaries (rather than re-slicing the finest
    // level into view-relative buckets every frame) is what keeps the
    // rendered peaks from flickering as the view pans/zooms by fractional
    // amounts - only whole buckets enter/leave at the edges, and each
    // bucket's min/max already accounts for every sample inside it.
    int level = 0;
    while (level + 1 < numLevels)
    {
        const double levelSamplesPerBlock = baseSamplesPerBlock * (double) (1 << level);
        const double blocksInViewAtLevel = viewLengthSamples / levelSamplesPerBlock;
        if (blocksInViewAtLevel <= (double) targetWidth)
            break;
        ++level;
    }

    const auto& levelBlocks = audioEngine.getMipLevel(level);
    if (levelBlocks.empty())
        return;

    const double levelSamplesPerBlock = baseSamplesPerBlock * (double) (1 << level);
    const int numLevelBlocks = (int) levelBlocks.size();

    // Fix the block WINDOW to targetWidth blocks (rather than exactly
    // however many floor()/ceil() of the precise view bounds happens to
    // produce) so textureWidth stays constant from one rebuild to the next
    // wherever there's enough file on both sides to allow it. Sizing the
    // texture from the raw floor/ceil instead made its width wobble by +/-1
    // texel practically every rebuild as the view shifted continuously,
    // which forced a full glTexImage2D REallocation (not just a data
    // upload) almost every time - an occasional reallocation stall landing
    // on the wrong frame is a very plausible source of the last bit of
    // "minor" flicker, distinct from anything already fixed above.
    int firstBlock = (int) std::floor(viewStartSamples / levelSamplesPerBlock);
    int windowBlocks = juce::jmin(targetWidth, numLevelBlocks);
    firstBlock = juce::jlimit(0, numLevelBlocks - windowBlocks, firstBlock);
    int lastBlockExclusive = firstBlock + windowBlocks;

    const int numVisibleBlocks = lastBlockExclusive - firstBlock;
    textureWidth = juce::jmin(numVisibleBlocks, safeMaxWidth);

    auto toByte = [](float v01) -> juce::uint8
    {
        return (juce::uint8) juce::jlimit(0, 255, (int) std::lround(v01 * 255.0f));
    };

    // Normally close to 1 (one texel per fixed bucket at the chosen level).
    // Only exceeds 1 in the rare case where even the coarsest level still
    // has more blocks in view than the texture can hold - grouping is still
    // done over fixed level-block indices, and every texel still covers its
    // whole span exhaustively, so it stays just as stable and just as safe
    // for the shader's single-sample lookup.
    const double blocksPerTexel = (double) numVisibleBlocks / (double) textureWidth;

    std::vector<juce::uint8> pixels((size_t) textureWidth * 4);
    std::vector<juce::uint8> midPixels((size_t) textureWidth * 4);

    for (int t = 0; t < textureWidth; ++t)
    {
        int i0 = firstBlock + (int) (t * blocksPerTexel);
        int i1 = firstBlock + juce::jmax((int) ((t + 1) * blocksPerTexel), (int) (t * blocksPerTexel) + 1);
        i0 = juce::jlimit(firstBlock, lastBlockExclusive - 1, i0);
        i1 = juce::jlimit(i0 + 1, lastBlockExclusive, i1);

        float minV = 1.0f, maxV = -1.0f, lowE = 0.0f, midE = 0.0f, highE = 0.0f;
        for (int i = i0; i < i1; ++i)
        {
            const auto& b = levelBlocks[(size_t) i];
            minV = juce::jmin(minV, b.minValue);
            maxV = juce::jmax(maxV, b.maxValue);
            lowE = juce::jmax(lowE, b.lowEnergy);
            midE = juce::jmax(midE, b.midEnergy);
            highE = juce::jmax(highE, b.highEnergy);
        }

        // min/max are -1..1, packed into 0..1 for 8-bit storage; unpacked back
        // to -1..1 in the shader. Energies are already 0..1.
        pixels[(size_t) t * 4 + 0] = toByte(minV * 0.5f + 0.5f);
        pixels[(size_t) t * 4 + 1] = toByte(maxV * 0.5f + 0.5f);
        pixels[(size_t) t * 4 + 2] = toByte(lowE);
        pixels[(size_t) t * 4 + 3] = toByte(highE);

        midPixels[(size_t) t * 4 + 0] = toByte(midE);
        midPixels[(size_t) t * 4 + 1] = 0;
        midPixels[(size_t) t * 4 + 2] = 0;
        midPixels[(size_t) t * 4 + 3] = 255;
    }

    // With the window above now fixed to targetWidth blocks (rather than
    // wobbling +/-1 with the exact view bounds), textureWidth stays constant
    // across rebuilds almost all the time - so reuse the existing GPU
    // storage with a data-only update (glTexSubImage2D) instead of asking
    // the driver to reallocate it (glTexImage2D) on every single rebuild.
    glBindTexture(GL_TEXTURE_2D, waveformTexture);
    if (textureWidth == allocatedTextureWidth)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureWidth, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glBindTexture(GL_TEXTURE_2D, midTexture);
    if (textureWidth == allocatedTextureWidth)
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, textureWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE, midPixels.data());
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, textureWidth, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, midPixels.data());

    allocatedTextureWidth = textureWidth;

    // The texture's bucket boundaries are snapped to whole fixed blocks
    // (floor/ceil above), so the sample range it actually covers is very
    // slightly wider than the precise [viewStart, viewStart+viewLength)
    // window - not the same thing. Record the texture's real span here so
    // the shader can map screen x to it exactly instead of assuming a 1:1
    // match; without this, that rounding offset changes as viewStart moves
    // continuously past each block boundary during a drag, which reads as
    // the waveform content subtly snapping/judder while panning.
    textureViewStart = firstBlock * levelSamplesPerBlock;
    textureViewLength = (double) numVisibleBlocks * levelSamplesPerBlock;
    textureDirty = false;
}

void WaveformComponent::renderOpenGL()
{
    // Times this whole function - texture rebuild (when one's needed) plus
    // the draw call - regardless of which return statement below is hit.
    const FrameTimerGuard frameTimerGuard(*this);

    juce::OpenGLHelpers::clear(params.backgroundColour);

    // Evaluated once, up front, and reused for BOTH the follow-playhead
    // view-centring below AND the drawn line further down. A prior version
    // centred the view from a separate 60Hz message-thread timer while the
    // line was drawn fresh every render frame - between two timer ticks the
    // line would legitimately drift away from centre as playback advanced,
    // then visibly snap back the instant the next tick recentred the view.
    // That drift is a fixed number of SAMPLES per tick, so it became a
    // larger and larger fraction of the visible window (and so more
    // visible) the further you zoomed in - which matches the "jumps
    // further the more you zoom in" symptom exactly. Computing both from
    // the identical reading in the same call removes the gap between them
    // entirely: the view is always centred on precisely the sample the
    // line is drawn at, every single frame.
    const double sampleRate = audioEngine.getSampleRate();
    const bool isPlayingNow = audioEngine.isPlaying();
    const double smoothedSeconds = playheadLineTracker.update(audioEngine.getPosition(), isPlayingNow);
    const double playheadSample = sampleRate > 0.0 ? smoothedSeconds * sampleRate : 0.0;

    if (followPlayhead && isPlayingNow && sampleRate > 0.0)
    {
        const juce::ScopedLock sl(dataLock);
        viewStart = playheadSample - viewLength * 0.5;
        clampView();
    }

    // Take one consistent snapshot of the view for the rest of this frame.
    // Without this, the message thread (mouse handling) could mutate
    // viewStart/viewLength partway through this method - e.g. between
    // building the texture from one view and computing the texture-to-
    // screen mapping from another - which is a one-frame misalignment each
    // time it happens.
    double localViewStart, localViewLength;
    {
        const juce::ScopedLock sl(dataLock);
        localViewStart = viewStart;
        localViewLength = viewLength;
    }

    // The texture window covers many more blocks than one frame's view
    // strictly needs (see uploadWaveformTexture), so most of the time the
    // new view is still fully covered by whatever's already uploaded - only
    // rebuild once it's about to slide outside that. Following now moves
    // the view every render frame (up to display refresh rate) rather than
    // a fixed 60Hz, so unconditionally rebuilding here would mean a full
    // CPU aggregation + GPU upload on every single frame instead of only
    // when the window genuinely needs to shift - and any of the resulting
    // per-rebuild variation is exactly what AA width/smoothing (which both
    // widen how much of each block's edge is antialiased) make more visible,
    // which is presumably why this read as worse with them turned up.
    if (localViewStart < textureViewStart || localViewStart + localViewLength > textureViewStart + textureViewLength)
        textureDirty = true;

    if (textureDirty)
        uploadWaveformTexture(localViewStart, localViewLength);

    if (shader == nullptr || textureWidth == 0)
        return;

    const double total = getTotalLength();
    if (total <= 0.0)
        return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    shader->use();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, waveformTexture);
    shader->setUniform("waveformTex", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, midTexture);
    shader->setUniform("midTex", 1);

    shader->setUniform("texWidth", (float) textureWidth);
    const float texMapScale = textureViewLength > 0.0 ? (float) (localViewLength / textureViewLength) : 1.0f;
    const float texMapOffset = textureViewLength > 0.0 ? (float) ((localViewStart - textureViewStart) / textureViewLength) : 0.0f;
    shader->setUniform("texMapOffset", texMapOffset);
    shader->setUniform("texMapScale", texMapScale);
    shader->setUniform("aaWidth", params.aaWidth);
    shader->setUniform("smoothing", params.smoothing);
    shader->setUniform("pixelHeight", (float) getHeight() * (float) openGLContext.getRenderingScale());
    shader->setUniform("pixelWidth", (float) getWidth() * (float) openGLContext.getRenderingScale());

    // playheadSample was already computed above (and used to centre the
    // view, when following) - reused here so the line is guaranteed to
    // land exactly at the view's centre while following, every frame.
    const bool playheadVisible = audioEngine.hasFileLoaded();
    const float playheadViewFrac = localViewLength > 0.0 ? (float) ((playheadSample - localViewStart) / localViewLength) : 0.0f;
    shader->setUniform("playheadViewFrac", playheadViewFrac);
    shader->setUniform("playheadVisible", playheadVisible ? 1.0f : 0.0f);
    shader->setUniform("centreLineAlpha", params.centreLineAlpha);

    auto toVec3 = [](juce::Colour c) {
        return std::array<float, 3>{ c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue() };
    };
    auto setColour = [&](const char* name, juce::Colour c) {
        auto v = toVec3(c);
        shader->setUniform(name, v[0], v[1], v[2]);
    };
    setColour("solidColour", params.solidColour);
    setColour("lowColour", params.lowFreqColour);
    setColour("midColour", params.midFreqColour);
    setColour("highColour", params.highFreqColour);
    setColour("backgroundColour", params.backgroundColour);
    setColour("playheadColour", params.playheadColour);
    shader->setUniform("lowAmount", params.lowFreqAmount);
    shader->setUniform("midAmount", params.midFreqAmount);
    shader->setUniform("highAmount", params.highFreqAmount);
    shader->setUniform("tintEnabled", params.tintingEnabled ? 1.0f : 0.0f);

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    auto positionAttrib = glGetAttribLocation(shader->getProgramID(), "position");
    glEnableVertexAttribArray((GLuint) positionAttrib);
    glVertexAttribPointer((GLuint) positionAttrib, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Wall-clock timing of just the draw call, for the frame-time readout.
    // This measures CPU-side submission/blocking time, not true isolated
    // GPU execution time (that needs GL timer query extensions, which
    // aren't reliably available under the GLES2-style context in use here)
    // - close enough to flag real regressions without extra extensions.
    const double drawStartMs = juce::Time::getMillisecondCounterHiRes();
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    const double drawCallMs = juce::Time::getMillisecondCounterHiRes() - drawStartMs;
    lastDrawCallMs = drawCallMs;
    if (drawCallMs > peakDrawCallMs.load())
        peakDrawCallMs = drawCallMs;

    glDisableVertexAttribArray((GLuint) positionAttrib);
}

void WaveformComponent::openGLContextClosing()
{
    glDeleteBuffers(1, &vertexBuffer);
    glDeleteTextures(1, &waveformTexture);
    glDeleteTextures(1, &midTexture);
    shader.reset();
}
