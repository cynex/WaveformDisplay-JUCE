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
        uniform float midPole;          // 0..1 pivot for how strongly midE reads in the blend - see WaveformParameters::midPole
        uniform float waveformHeight;   // nominal height scale applied BEFORE the amplitude boost - see WaveformParameters::waveformHeight
        uniform float amplitudeAmount;  // 0..1 mix towards the exaggerated amplitude-driven height boost - see WaveformParameters::amplitudeAmount
        uniform float amplitudeColorAmount; // 0..4 strength of the additive colour glow - see WaveformParameters::amplitudeColorAmount
        uniform float amplitudeGlowRadius; // pixels - how far the outer halo bleeds past the waveform's silhouette - see WaveformParameters::amplitudeGlowRadius
        uniform float amplitudePulse;   // 0..1, CPU-smoothed overall amplitude AT THE PLAYHEAD right now - see WaveformComponent::AmplitudePulseTracker
        uniform float amplitudeRangeNorm; // 0..1 fraction of the view width the amplitude effect is allowed to fade out over, centred on the playhead - see WaveformParameters::amplitudeRange
        uniform float amplitudeSlope;   // gamma exponent for the amplitude-driving curve - see WaveformParameters::amplitudeSlope / ampCurve
        uniform vec3 amplitudeBandWeights; // (low, mid, high) blend weights, summing to 1, from amplitudeMinFrequencyHz/amplitudeMaxFrequencyHz - see WaveformComponent::computeAmplitudeBandWeights
        uniform float tintEnabled;      // 0 or 1 - master switch for low/mid/high tinting
        uniform vec3 backgroundColour;
        uniform vec3 playheadColour;
        uniform float playheadViewFrac; // playback position, 0..1 across the CURRENT view
        uniform float playheadVisible;  // 0 or 1
        uniform float centreLineAlpha;  // opacity of the solid white zero-amplitude axis line, 0..1
        uniform vec3 amplitudeColour;   // additive glow colour driven by the amplitude pulse - see WaveformParameters::amplitudeColour

        // Fixed ceiling on how far a fully loud, fully-pulsing block right
        // at the playhead can be inflated when effectiveHeightBoost == 1.0 -
        // deliberately a large, exaggerated multiplier (see the "fun, not
        // accurate" intent below), not something exposed as its own slider.
        // amplitudeAmount's whole 0..1 range is a mix towards/away from
        // this fixed curve, not a multiplier of its own that could stack
        // with a separate "how much" control.
        const float kMaxAmplitudeMultiplier = 3.0;

        // Remaps x (0..1) through a logarithmic curve that rises steeply
        // near 0 and flattens out towards 1 - used for the amplitude
        // effect's distance-from-playhead falloff (sampleWaveform's
        // rangeCoverage). NOTE: for small x, log(1+x*steepness) is
        // approximately LINEAR in x (log(1+t) ~= t for small t) - that's
        // fine for a spatial falloff, which only needs a soft tail, but it
        // means this curve does NOT give a strongly lifted response to
        // small inputs - see ampCurve below, which is what the amplitude
        // effect itself now uses specifically because a real boost for
        // quiet material is the whole point there.
        float logCurve(float x, float steepness)
        {
            float xClamped = clamp(x, 0.0, 1.0);
            return log(1.0 + xClamped * steepness) / log(1.0 + steepness);
        }

        // Remaps x (0..1) through a gamma/power curve (x^(1/gamma)) rather
        // than the logarithmic one above - chosen specifically because a
        // log curve's response to genuinely QUIET input is close to linear
        // (see logCurve's note), so subtle/quiet audio barely moved the
        // boost/glow at all, reading as "no action" until the audio was
        // already fairly loud. A gamma curve with gamma > 1 lifts small x
        // MUCH more aggressively than log does - x=0.01 at gamma=4 maps to
        // ~0.32, versus ~0.09 under the old log curve at a comparable
        // steepness - so even quiet passages now produce a clearly visible
        // pulse, while gamma==1 is the identity (no lift at all) and the
        // curve still always reaches exactly 1.0 at x==1 (a hit still reads
        // as full strength, never overshooting). This is the standard
        // "gamma correction" shape (same one used to brighten shadows in
        // images) - reads as more dramatic/"exponential-feeling" than the
        // log curve specifically because of how much harder it lifts the
        // low end, not because it grows faster at the top.
        float ampCurve(float x, float gamma)
        {
            float xClamped = clamp(x, 0.0, 1.0);
            float gammaSafe = max(gamma, 0.0001);
            return pow(xClamped, 1.0 / gammaSafe);
        }

        // effectiveHeightBoost and effectiveGlowAmount are the FINAL,
        // already-combined strengths for this fragment - amplitudeAmount
        // (0..1, the height-boost slider) or amplitudeColorAmount (0..4,
        // the colour-glow slider, allowed past 1 for extra-bright pulses)
        // respectively, each times amplitudePulse (the CPU-smoothed
        // envelope of amplitude at the playhead right now, computed once in
        // sampleWaveform/main and reused here) times however much this
        // fragment's rangeCoverage (logarithmic distance-from-playhead
        // falloff, also computed once in sampleWaveform) lets through.
        // coverageFor itself stays ignorant of pulse/range - it just
        // applies whatever final amounts it's handed. Kept as two SEPARATE
        // inputs (not one shared amount) specifically so the colour glow
        // can be pushed well past what the height boost is doing, per the
        // "its own parameter, goes past 1" ask - the underlying pulse/range
        // timing they both ride on stays shared, so the two still read as
        // one coordinated pulse rather than two independently-timed effects.
        float coverageFor(vec4 texel, float midE, float y, float edge, float smoothingAmount, float waveformHeightAmount, float effectiveHeightBoost, float effectiveGlowAmount, out float glowOut, out float haloOut)
        {
            float minV = texel.r * 2.0 - 1.0;
            float maxV = texel.g * 2.0 - 1.0;

            // The value that actually DRIVES the boost/glow is a blend of
            // this block's low/mid/high band energies (texel.b, midE,
            // texel.a - each already 0..1), weighted by amplitudeBandWeights
            // (from amplitudeMinFrequencyHz/amplitudeMaxFrequencyHz), rather
            // than the block's raw broadband peak amplitude - so narrowing
            // that frequency range (e.g. towards the mid band) makes the
            // effect track energy in just that range instead of reacting to
            // the whole spectrum. amplitudeSlope (user-controlled - now a
            // gamma exponent, see ampCurve) sets how strongly quiet input
            // gets lifted before it becomes boost/glow strength.
            float ampBand = clamp(texel.b * amplitudeBandWeights.x + midE * amplitudeBandWeights.y + texel.a * amplitudeBandWeights.z, 0.0, 1.0);
            float ampCurved = ampCurve(ampBand, amplitudeSlope);

            // Nominal height scale, applied BEFORE the boost multiplier -
            // shrinking the waveform's baseline height first is what gives
            // the boost actual headroom to visibly inflate a block without
            // immediately hitting the +/-1 clamp below: a block already
            // sitting near +/-1 at full nominal height has nowhere left to
            // grow, so the boost effect reads as "crushed"/invisible right
            // where it should be most dramatic (the loudest moments). See
            // WaveformParameters::waveformHeight.
            minV *= waveformHeightAmount;
            maxV *= waveformHeightAmount;

            // "Amplitude boost": inflate this block's rendered envelope
            // height, so loud moments visibly bulge the waveform's
            // silhouette outward rather than just tinting a different
            // colour - a deliberately exaggerated, fun/intuitive way to see
            // a beat move through the waveform, not an accurate amplitude
            // readout. effectiveHeightBoost is a straight mix between "no
            // change at all" (0) and the full amplitude-modulated curve
            // (1), rather than its own multiplier - so 0 (amplitudeAmount,
            // amplitudePulse, or rangeCoverage each independently) is
            // guaranteed to be exactly a no-op.
            float fullBoost = 1.0 + ampCurved * kMaxAmplitudeMultiplier;
            float boost = mix(1.0, fullBoost, clamp(effectiveHeightBoost, 0.0, 1.0));

            // NOT clamped to 1 - amplitudeColorAmount is allowed up to 4,
            // and the glow is a purely additive light effect (see
            // sampleWaveform), so there's no "overflow" concern the way
            // there is for the height boost's +/-1 amplitude range below;
            // only guarded against going negative.
            glowOut = ampCurved * max(effectiveGlowAmount, 0.0);

            // Clamped back to the real -1..1 amplitude range afterwards, so
            // an over-boosted block just reads as "full height" during a
            // loud hit instead of actually overflowing the component.
            minV = clamp(minV * boost, -1.0, 1.0);
            maxV = clamp(maxV * boost, -1.0, 1.0);

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
            float coverage = 1.0 - smoothstep(-feather, feather, dist);

            // Outer glow: an additive halo flooding the NEGATIVE SPACE
            // around the waveform's own silhouette, using the exact same
            // amplitude-driven glowOut as its gain - so the halo
            // brightens/dims in lockstep with the interior colour glow,
            // both riding the one shared amplitude pulse (see the class
            // comment on effectiveGlowAmount). Falls off exponentially with
            // distance PAST the edge (amplitudeGlowRadius, in pixels,
            // converted to the same amplitude-unit space as `dist` via
            // pixelToAmp - matching how aaWidth/edge are converted
            // elsewhere), and is scaled by (1.0 - coverage) so it only
            // actually shows up where the waveform's own shape has already
            // faded towards background - without that, a wide radius would
            // double up with (and could outshine) the interior glow already
            // applied on top of the solid shape.
            //
            // kHaloIntensityMultiplier pushes the halo noticeably brighter
            // than glowOut's raw value on its own - the interior glow is
            // deliberately subtle (it's tinting an already-solid,
            // already-coloured shape, so a little goes a long way), but the
            // SAME raw strength spread out across open background space
            // reads as much fainter unless boosted - this is what makes the
            // ambient "glow filling the space around the waveform" actually
            // visible instead of a barely-there edge fringe.
            const float kHaloIntensityMultiplier = 3.0;
            float pixelToAmpForGlow = 2.0 / max(pixelHeight, 1.0);
            float glowRadiusAmp = max(amplitudeGlowRadius * pixelToAmpForGlow, 0.0001);
            float distOutside = max(dist, 0.0);
            float haloFalloff = exp(-distOutside / glowRadiusAmp);
            haloOut = glowOut * haloFalloff * (1.0 - coverage) * kHaloIntensityMultiplier;

            return coverage;
        }

        // Everything x-position-dependent up through the waveform's own
        // colour (i.e. excluding the centre line and playhead, which have
        // their own dedicated 1px AA and are drawn once, not supersampled -
        // see main()). Factored out so main() can call it twice at
        // sub-pixel-offset x positions and average the results for 2x
        // horizontal supersampling: each screen pixel's block-boundary
        // blend, coverage, and frequency tint are all functions of x that
        // can vary with high spatial frequency (a fast zoom-out packs many
        // blocks per pixel), and evaluating them only once at the pixel
        // centre can alias/shimmer as that sub-pixel sample point drifts
        // frame to frame - most visible as the SAME kind of edge flicker
        // the block-boundary blend above already fixes for panning, just
        // reintroduced at a finer scale by zooming instead.
        vec3 sampleWaveform(float xNorm, float y, out float outOfRangeCoverage)
        {
            float texU = texMapOffset + xNorm * texMapScale;

            // Texel-centre convention: texel i's centre sits at (i+0.5)/texWidth,
            // so this recovers a continuous texel index whose fractional part
            // is exactly how far the fragment sits from idx0's centre towards idx1's.
            float texIndexF = texU * texWidth - 0.5;
            float idx0 = floor(texIndexF);
            float frac = texIndexF - idx0;
            float idx1 = idx0 + 1.0;

            // How far (in texels) idx0/idx1 fall outside the texture's
            // actual [0, texWidth) coverage, BEFORE clamping them into
            // range below. Non-zero here means this fragment's mapped
            // position isn't really backed by the currently-uploaded
            // texture at all - most commonly because the view has moved
            // (panning, zooming, or scrolling during playback) faster than
            // the background rebuild thread has kept up, so the texture
            // still on screen was built for a narrower/offset window than
            // the current view. Clamping idx0/idx1 alone would just repeat
            // whichever real block sits at the texture's edge across that
            // whole region, which reads as a solid pixelated/stretched
            // smear - outOfRangeCoverage below fades that region to the
            // background colour instead, which reads as "not loaded yet"
            // rather than as wrong waveform content.
            outOfRangeCoverage = max(
                smoothstep(0.0, 1.0, -texIndexF),
                smoothstep(0.0, 1.0, texIndexF - (texWidth - 1.0)));

            idx0 = clamp(idx0, 0.0, texWidth - 1.0);
            idx1 = clamp(idx1, 0.0, texWidth - 1.0);

            vec4 texel0 = texture2D(waveformTex, vec2((idx0 + 0.5) / texWidth, 0.5));
            vec4 texel1 = texture2D(waveformTex, vec2((idx1 + 0.5) / texWidth, 0.5));
            float midE0 = texture2D(midTex, vec2((idx0 + 0.5) / texWidth, 0.5)).r;
            float midE1 = texture2D(midTex, vec2((idx1 + 0.5) / texWidth, 0.5)).r;

            // Convert the AA width parameter (pixel-space) into the same
            // normalised amplitude units used above. Smoothing is applied
            // per-block, scaled by each block's own height (see coverageFor).
            float pixelToAmp = 2.0 / max(pixelHeight, 1.0);
            float edge = max(aaWidth * pixelToAmp, 0.0001);

            // Spatial mask for the amplitude effect: how much of it is let
            // through at THIS fragment's x position, based on distance from
            // the playhead in view-fraction space, falling off
            // LOGARITHMICALLY rather than linearly - dNorm (0 at the
            // playhead, 1 at the edge of amplitudeRangeNorm) is remapped
            // through log(1 + dNorm*k)/log(1+k), which drops steeply right
            // next to the playhead and then flattens out over the rest of
            // the range, instead of a uniform linear fade. That reads as a
            // distinct pulse hugging the playhead with a long soft tail,
            // rather than a diffuse wash that fades evenly across the whole
            // range. k (kRangeLogSteepness) is a fixed shape constant, not
            // exposed as its own control - amplitudeRangeNorm alone (from
            // the "Amplitude Range" slider) still fully determines how far
            // the effect reaches. max(..., 0.0001) keeps the divide well
            // clear of zero at the slider's own minimum.
            const float kRangeLogSteepness = 12.0;
            float rangeWidth = max(amplitudeRangeNorm, 0.0001);
            float dxFromPlayhead = abs(xNorm - playheadViewFrac);
            float dNorm = clamp(dxFromPlayhead / rangeWidth, 0.0, 1.0);
            float rangeCoverage = 1.0 - logCurve(dNorm, kRangeLogSteepness);

            // The final, fully-combined strengths for this fragment: each
            // slider (amplitudeAmount for height, amplitudeColorAmount for
            // colour - see coverageFor's comment for why they're kept
            // separate), times the current amplitude pulse (0 outside of
            // playback - see AmplitudePulseTracker), times how much the
            // spatial mask above lets through here. Zero in ANY of those
            // three (for a given one) means that effect is exactly off for
            // this fragment.
            float effectiveHeightBoost = amplitudeAmount * amplitudePulse * rangeCoverage;
            float effectiveGlowAmount = amplitudeColorAmount * amplitudePulse * rangeCoverage;

            float glow0, glow1, halo0, halo1;
            float coverage0 = coverageFor(texel0, midE0, y, edge, smoothing, waveformHeight, effectiveHeightBoost, effectiveGlowAmount, glow0, halo0);
            float coverage1 = coverageFor(texel1, midE1, y, edge, smoothing, waveformHeight, effectiveHeightBoost, effectiveGlowAmount, glow1, halo1);

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
            float glow = mix(glow0, glow1, blend);
            float halo = mix(halo0, halo1, blend);
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
            // midPole warps midE through a gamma curve pivoting at the
            // chosen point (an input equal to midPole always maps to an
            // output of 0.5) BEFORE it's weighted by midAmount - a purely
            // visual reshaping of how strongly a given mid-energy reading
            // presents, not a change to the energy value analysed from the
            // audio itself. clamp() keeps log() away from 0/negative for
            // pole values right at the ends of the slider.
            float midPoleClamped = clamp(midPole, 0.001, 0.999);
            float midGamma = log(0.5) / log(midPoleClamped);
            float midEWarped = pow(clamp(midE, 0.0, 1.0), midGamma);

            float wLow = max(lowE * lowAmount, 0.0);
            float wMid = max(midEWarped * midAmount, 0.0);
            float wHigh = max(highE * highAmount, 0.0);
            float wSum = wLow + wMid + wHigh;
            vec3 freqBlend = wSum > 0.0001 ? (lowColour * wLow + midColour * wMid + highColour * wHigh) / wSum
                                            : vec3(1.0);

            vec3 tint = solidColour * mix(vec3(1.0), freqBlend, tintEnabled);

            // Additive amplitude glow - added directly on top of the
            // frequency tint (not mixed/multiplied), so it brightens
            // whatever colour is already there rather than replacing or
            // blending with it, and applies regardless of tintEnabled since
            // it's a separate effect from the low/mid/high tinting. Driven
            // by amplitudeColorAmount's own effectiveGlowAmount (see
            // coverageFor/sampleWaveform) - a SEPARATE strength from the one
            // that inflates the waveform's height, but riding the same
            // amplitude-pulse/range timing, so the colour glow and the
            // height "punch" always pulse together even though their
            // magnitudes can differ.
            tint += amplitudeColour * glow;

            vec3 outColour = mix(backgroundColour, tint, coverage);

            // Fade out-of-range fragments back to the background colour -
            // see outOfRangeCoverage above.
            outColour = mix(outColour, backgroundColour, outOfRangeCoverage);

            // Outer glow halo, additive on top of everything above (so it
            // brightens whatever's already there - background, or the
            // faded edge of the waveform itself - rather than replacing
            // it). Only meaningfully non-zero where coverage is low (see
            // coverageFor), i.e. outside/at the edge of the silhouette, so
            // this doesn't double up with the interior glow already folded
            // into tint. Suppressed by outOfRangeCoverage too, so the halo
            // doesn't paint into regions the texture hasn't loaded for yet.
            outColour += amplitudeColour * halo * (1.0 - outOfRangeCoverage);

            return outColour;
        }

        void main()
        {
            float y = (vUv.y * 2.0 - 1.0);

            // 2x horizontal supersampling: evaluate the waveform colour at
            // two sub-pixel-offset x positions (a quarter pixel either side
            // of centre - the standard 2-tap offset for this kind of
            // box-filtered supersampling) and average them, rather than a
            // single sample at the pixel centre. See sampleWaveform's
            // comment for what this is actually fixing.
            //
            // Always on, unconditionally - an earlier version skipped the
            // second sample on frames where the view hadn't moved, as an
            // optimisation. Removed: switching between the two-sample
            // average and a single centred sample changes the anti-aliased
            // edge coverage by a small but visible amount, which read as a
            // pop/flicker right at the moment a click-drag transitioned
            // from a static frame into a moving one - worse than the cost
            // it was saving.
            float pixelToNormX = 1.0 / max(pixelWidth, 1.0);
            float sampleOffset = pixelToNormX * 0.25;

            float outOfRangeA, outOfRangeB;
            vec3 colourA = sampleWaveform(vUv.x - sampleOffset, y, outOfRangeA);
            vec3 colourB = sampleWaveform(vUv.x + sampleOffset, y, outOfRangeB);
            vec3 outColour = (colourA + colourB) * 0.5;

            // Solid white 1px line through the zero-amplitude axis (drawn
            // under the playhead line, and under the waveform - it should
            // read as sitting behind/through the waveform, not painted over
            // it), opacity controlled by centreLineAlpha. Drawn once at the
            // pixel centre, not supersampled - it's a fixed, dead-flat
            // horizontal band with its own 1px AA already, so there's
            // nothing horizontally high-frequency about it to alias.
            if (centreLineAlpha > 0.0)
            {
                float pixelToNormY = 1.0 / max(pixelHeight, 1.0);
                float centreLineHalfWidth = 1.0 * pixelToNormY;
                float dyCentre = abs(vUv.y - 0.5);
                float centreLineCoverage = 1.0 - smoothstep(centreLineHalfWidth * 0.5, centreLineHalfWidth * 1.5, dyCentre);
                outColour = mix(outColour, vec3(1.0), centreLineCoverage * centreLineAlpha);
            }

            // Playhead: a thin anti-aliased vertical line at the current
            // playback position. Also drawn once at the pixel centre for the
            // same reason as the centre line - it's independent of aaWidth
            // and already gets its own crisp 1px AA below, deliberately so
            // it always reads as a sharp line regardless of how soft the
            // waveform's own edges are set to.
            if (playheadVisible > 0.5)
            {
                float lineHalfWidth = 1.0 * pixelToNormX;
                float dx = abs(vUv.x - playheadViewFrac);
                float lineCoverage = 1.0 - smoothstep(lineHalfWidth * 0.5, lineHalfWidth * 1.5, dx);
                outColour = mix(outColour, playheadColour, lineCoverage);
            }

            gl_FragColor = vec4(outColour, 1.0);
        }
    )";
}

WaveformComponent::WaveformComponent(AudioEngine& engine)
    : juce::Thread("WaveformTextureBuilder"), audioEngine(engine)
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
    //
    // Only actually WANTED while something is continuously animating
    // (playback/scratching moving the playhead, or follow-playhead moving
    // the view with it) - timerCallback() below flips it on/off to match
    // AudioEngine::isPlaying(), so an idle, paused view stops redrawing
    // every vsync for no reason. Discrete changes while idle (panning,
    // zooming, resizing, editing parameters) stay responsive without it -
    // each already calls triggerRepaint() itself (setViewRange,
    // setParameters, the background build thread's completion signal).
    // Starts false: nothing is loaded/playing yet at construction.
    openGLContext.setContinuousRepainting(false);
    // JUCE defaults this to 1 (vsync on) already; set it explicitly so
    // continuous repainting, whenever it's on, is definitely paced by the
    // display refresh and not free-running (which can tear/look
    // inconsistent) on whatever platform/driver combination this ends up
    // running on.
    openGLContext.setSwapInterval(1);
    // Also drives the continuous-repainting on/off switch above, and
    // periodically recentres the view during follow-playhead, which has to
    // happen on the message thread (setViewRange touches the scrollbar
    // Component).
    startTimerHz(60);

    startThread();
}

WaveformComponent::~WaveformComponent()
{
    // Wake the worker (it normally blocks indefinitely on buildRequestEvent,
    // which stopThread()'s own exit signal doesn't touch) so it notices
    // threadShouldExit() and unwinds before anything it might still be
    // reading (audioEngine, the tile cache) goes away.
    signalThreadShouldExit();
    buildRequestEvent.signal();
    stopThread(2000);

    openGLContext.detach();
}

void WaveformComponent::notifyFileChanged()
{
    // The tile cache is owned solely by the background build thread now
    // (see its class comment) - it notices a new file itself, via
    // AudioEngine::getLoadGeneration(), the next time it runs a build,
    // rather than being cleared from here across threads. This just makes
    // sure a build actually gets requested.
    textureDirty = true;
}

void WaveformComponent::clearTileCache()
{
    tileCache.clear();
    tileLru.clear();
    tileLruPos.clear();
}

const WaveformComponent::TileData& WaveformComponent::getOrBuildTile(int level, int tileIndex, const std::vector<WaveformBlock>& levelBlocks)
{
    const auto key = tileKey(level, tileIndex);

    auto found = tileCache.find(key);
    if (found != tileCache.end())
    {
        // Touch: move this key to the front of the LRU list so it's the
        // last thing evicted.
        tileLru.splice(tileLru.begin(), tileLru, tileLruPos[key]);
        return found->second;
    }

    const int blockStart = tileIndex * kTileBlocks;
    const int blockEnd = juce::jmin(blockStart + kTileBlocks, (int) levelBlocks.size());
    const int numBlocks = juce::jmax(0, blockEnd - blockStart);

    auto toByte = [](float v01) -> juce::uint8
    {
        return (juce::uint8) juce::jlimit(0, 255, (int) std::lround(v01 * 255.0f));
    };

    TileData tile;
    tile.numBlocks = numBlocks;
    tile.waveform.resize((size_t) numBlocks * 4);
    tile.mid.resize((size_t) numBlocks * 4);

    for (int i = 0; i < numBlocks; ++i)
    {
        const auto& b = levelBlocks[(size_t) (blockStart + i)];
        tile.waveform[(size_t) i * 4 + 0] = toByte(b.minValue * 0.5f + 0.5f);
        tile.waveform[(size_t) i * 4 + 1] = toByte(b.maxValue * 0.5f + 0.5f);
        tile.waveform[(size_t) i * 4 + 2] = toByte(b.lowEnergy);
        tile.waveform[(size_t) i * 4 + 3] = toByte(b.highEnergy);

        tile.mid[(size_t) i * 4 + 0] = toByte(b.midEnergy);
        tile.mid[(size_t) i * 4 + 1] = 0;
        tile.mid[(size_t) i * 4 + 2] = 0;
        tile.mid[(size_t) i * 4 + 3] = 255;
    }

    if (tileCache.size() >= kMaxCachedTiles && !tileLru.empty())
    {
        const auto evictKey = tileLru.back();
        tileLru.pop_back();
        tileLruPos.erase(evictKey);
        tileCache.erase(evictKey);
    }

    tileLru.push_front(key);
    tileLruPos[key] = tileLru.begin();
    auto inserted = tileCache.emplace(key, std::move(tile));
    return inserted.first->second;
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

    // Explicit rather than relying on the resize itself implicitly causing
    // a repaint - true often enough, but not guaranteed on every platform,
    // and now that continuous repainting is off while idle (see the
    // constructor), this is the only thing that would otherwise ask for a
    // redraw at all.
    openGLContext.triggerRepaint();
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

void WaveformComponent::computeAmplitudeBandWeights(float minHz, float maxHz, float& lowWeight, float& midWeight, float& highWeight)
{
    // Matches AudioEngine::analyse's fixed cutoffs exactly - these aren't
    // independently tunable here, only the caller's [minHz, maxHz] window
    // relative to them is.
    constexpr float lowMidEdgeHz  = 300.0f;
    constexpr float midHighEdgeHz = 3000.0f;
    constexpr float topHz         = 20000.0f; // effectively "top of the analysed range" for overlap purposes

    minHz = juce::jlimit(20.0f, topHz, minHz);
    maxHz = juce::jlimit(minHz, topHz, maxHz);

    auto overlap = [](float a0, float a1, float b0, float b1)
    {
        return juce::jmax(0.0f, juce::jmin(a1, b1) - juce::jmax(a0, b0));
    };

    const float overlapLow  = overlap(minHz, maxHz, 0.0f, lowMidEdgeHz);
    const float overlapMid  = overlap(minHz, maxHz, lowMidEdgeHz, midHighEdgeHz);
    const float overlapHigh = overlap(minHz, maxHz, midHighEdgeHz, topHz);
    const float total = overlapLow + overlapMid + overlapHigh;

    if (total <= 0.0001f)
    {
        // minHz==maxHz landed exactly on a band edge, or some other
        // degenerate case - fall back to an even split rather than
        // dividing by (near) zero.
        lowWeight = midWeight = highWeight = 1.0f / 3.0f;
        return;
    }

    lowWeight  = overlapLow  / total;
    midWeight  = overlapMid  / total;
    highWeight = overlapHigh / total;
}

float WaveformComponent::sampleAmplitudeAtSample(double sampleIndex) const
{
    auto pyramid = audioEngine.getMipPyramid();
    if (pyramid == nullptr || pyramid->empty())
        return 0.0f;

    const auto& finestLevel = pyramid->front();
    if (finestLevel.empty())
        return 0.0f;

    const int samplesPerBlock = juce::jmax(1, audioEngine.getNumSamplesPerBlock());
    const int numBlocks = (int) finestLevel.size();
    const int centreBlock = (int) (sampleIndex / (double) samplesPerBlock);

    // A small window of blocks, not just the one the playhead is exactly
    // over - a single block is ~samplesPerBlock/sampleRate seconds wide
    // (often under 15ms), narrow enough that a real loud hit landing just
    // outside it would otherwise be missed entirely for a frame or two.
    // max() (not average) so a nearby loud block isn't diluted by quieter
    // neighbours either.
    constexpr int windowRadiusBlocks = 3;
    const int first = juce::jlimit(0, numBlocks - 1, centreBlock - windowRadiusBlocks);
    const int last  = juce::jlimit(0, numBlocks - 1, centreBlock + windowRadiusBlocks);

    float lowWeight, midWeight, highWeight;
    computeAmplitudeBandWeights(params.amplitudeMinFrequencyHz, params.amplitudeMaxFrequencyHz, lowWeight, midWeight, highWeight);

    float maxBandAmplitude = 0.0f;
    for (int i = first; i <= last; ++i)
    {
        const auto& b = finestLevel[(size_t) i];
        const float bandValue = b.lowEnergy * lowWeight + b.midEnergy * midWeight + b.highEnergy * highWeight;
        maxBandAmplitude = juce::jmax(maxBandAmplitude, bandValue);
    }

    return juce::jlimit(0.0f, 1.0f, maxBandAmplitude);
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

        // Continuous repainting is off whenever nothing's playing (see the
        // constructor) - moving the playhead line while paused is a
        // discrete change like a pan or zoom, so it needs its own explicit
        // repaint the same way those get one, or the new position wouldn't
        // actually show up on screen until something else (like a pan)
        // happened to ask for a redraw anyway.
        openGLContext.triggerRepaint();
    }
}

void WaveformComponent::mouseDown(const juce::MouseEvent& e)
{
    dragStartViewStart = viewStart;
    dragStartMouse = e.position;
    draggedPastClickThreshold = false;

    // Whether this whole gesture scratches (turntable-style: the audio
    // engine chases a continuously-updated target position, so playback
    // speed/direction/pitch follow the mouse) or pans is decided once,
    // here, from playback state at the moment the click lands - not
    // re-evaluated every mouseDrag - so starting or stopping playback
    // mid-drag can't switch a gesture's meaning out from under the user
    // partway through it.
    scrubbingThisDrag = audioEngine.hasFileLoaded() && audioEngine.isPlaying();

    if (scrubbingThisDrag)
    {
        audioEngine.beginScratch();
        scratchTargetSeconds = audioEngine.getPosition();
    }
}

void WaveformComponent::mouseDrag(const juce::MouseEvent& e)
{
    const double total = getTotalLength();
    if (total <= 0.0 || getWidth() <= 0)
        return;

    if (scrubbingThisDrag)
    {
        // Relative, not absolute: the target moves by however far the
        // cursor moved since the LAST event, converted to samples at the
        // current zoom level, rather than jumping straight to wherever the
        // cursor now sits on the waveform. Inverted (dragging right moves
        // the target BACKWARDS) to match a turntable, where pulling the
        // record towards you (which reads as a leftward drag once you're
        // looking at time increasing to the right) is what scratches it
        // backwards. The actual audible speed/smoothness of the scratch
        // comes from ScratchSource's own follow filter continuously chasing
        // this target - see its comment - not from anything done here.
        const double samplesPerPixel = viewLength / (double) getWidth();
        const float dxPixels = e.position.x - dragStartMouse.x;
        dragStartMouse = e.position;

        const double sampleRate = audioEngine.getSampleRate();
        if (sampleRate > 0.0)
        {
            scratchTargetSeconds -= (dxPixels * samplesPerPixel) / sampleRate;
            scratchTargetSeconds = juce::jlimit(0.0, total / sampleRate, scratchTargetSeconds);
            audioEngine.setScratchTargetSeconds(scratchTargetSeconds);
        }

        // Follow-playhead (if enabled) recentres the view on the new
        // playback position every render frame on its own - see
        // renderOpenGL - so scratching doesn't need to touch the view here
        // itself, just keep moving the target position.
        return;
    }

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
    // Hand playback back to the transport, resuming from wherever
    // scratching left the position.
    if (scrubbingThisDrag)
    {
        audioEngine.endScratch();
        return;
    }

    // A genuine click (not a pan) seeks the playhead to wherever the cursor
    // landed - deferred to mouseUp rather than mouseDown so a click-drag pan
    // never has its start point misread as a seek (see mouseDrag).
    if (!draggedPastClickThreshold)
        seekToScreenX(e.position.x);
}

void WaveformComponent::timerCallback()
{
    // Continuous repainting (see the constructor's comment) is only worth
    // paying for while something is actually animating on its own, without
    // any further input - playback or scratching moving the playhead, and
    // (while enabled) the view following it. Checked here rather than
    // wherever play/scratch state changes so there's exactly one place that
    // owns the on/off decision, and so it stays correct without every
    // caller of play()/beginScratch()/etc. needing to remember to toggle it.
    const bool shouldBeContinuous = audioEngine.isPlaying();
    if (shouldBeContinuous != continuousRepaintActive)
    {
        openGLContext.setContinuousRepainting(shouldBeContinuous);
        continuousRepaintActive = shouldBeContinuous;
    }

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

    // A texture width equal to the raw (finest) analysis block count (which
    // can be 100,000+ for a full song) silently exceeds GL_MAX_TEXTURE_SIZE
    // on every GPU (usually 8192-16384) - glTexImage2D then fails and the
    // texture is left uninitialised. Only queryable with a current GL
    // context, so it's read once here (GL thread) and cached for the
    // background build thread (which has no GL context) to read.
    GLint maxTexSize = 4096;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
    safeMaxTextureWidth = juce::jmin((int) maxTexSize, 8192);

    glGenBuffers(1, &vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);

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
    //
    // Two slots each, for double buffering - see the member comment on
    // waveformTextures.
    glGenTextures(2, waveformTextures);
    glGenTextures(2, midTextures);
    for (int slot = 0; slot < 2; ++slot)
    {
        glBindTexture(GL_TEXTURE_2D, waveformTextures[slot]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Second texture for mid-band energy - waveformTextures' RGBA8
        // channels were already fully spoken for (min/max/lowEnergy/highEnergy).
        glBindTexture(GL_TEXTURE_2D, midTextures[slot]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        allocatedTextureWidths[slot] = -1;
    }

    activeTextureSlot = 0;
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

void WaveformComponent::run()
{
    // Blocks here until the GL thread has a rebuild for us (or wants us to
    // exit) - see the class comment and the member comments on
    // buildRequestEvent/pendingRequest/readyResult for the overall design.
    while (!threadShouldExit())
    {
        buildRequestEvent.wait(-1);
        if (threadShouldExit())
            break;

        BuildRequest request;
        {
            const juce::ScopedLock sl(requestLock);
            if (!hasPendingRequest)
                continue; // spurious wake, or already consumed - nothing to do
            request = pendingRequest;
            hasPendingRequest = false;
        }

        // A new file means block index N at any given level no longer means
        // the same audio it meant a moment ago, so every cached tile is now
        // stale - detected here (rather than the message thread reaching
        // across to clear the cache itself) because the cache is otherwise
        // exclusively this thread's.
        const int loadGen = audioEngine.getLoadGeneration();
        if (loadGen != workerCachedLoadGeneration)
        {
            clearTileCache();
            workerCachedLoadGeneration = loadGen;
        }

        BuildResult result = buildTextureData(request);
        if (result.textureWidth <= 0)
            continue;

        {
            const juce::ScopedLock sl(resultLock);
            readyResult = std::move(result);
            hasReadyResult = true;
        }

        // Wake the GL thread so the finished texture gets uploaded and shown
        // promptly rather than waiting for its next incidental repaint.
        openGLContext.triggerRepaint();
    }
}

WaveformComponent::BuildResult WaveformComponent::buildTextureData(const BuildRequest& request)
{
    BuildResult result;
    result.generation = request.generation;

    // A shared_ptr snapshot, not a reference - so a file reload on the
    // message thread publishing a new pyramid mid-build can't pull this
    // one out from under us; see AudioEngine::getMipPyramid().
    auto pyramid = audioEngine.getMipPyramid();
    if (pyramid == nullptr || pyramid->empty())
        return result;

    const double baseSamplesPerBlock = juce::jmax(1, audioEngine.getNumSamplesPerBlock());
    const int numLevels = (int) pyramid->size();

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
        const double blocksInViewAtLevel = request.viewLength / levelSamplesPerBlock;
        if (blocksInViewAtLevel <= (double) request.targetWidth)
            break;
        ++level;
    }

    const auto& levelBlocks = (*pyramid)[(size_t) level];
    if (levelBlocks.empty())
        return result;

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
    int firstBlock = (int) std::floor(request.viewStart / levelSamplesPerBlock);
    int windowBlocks = juce::jmin(request.targetWidth, numLevelBlocks);
    firstBlock = juce::jlimit(0, numLevelBlocks - windowBlocks, firstBlock);
    int lastBlockExclusive = firstBlock + windowBlocks;

    const int numVisibleBlocks = lastBlockExclusive - firstBlock;
    const int textureWidthLocal = juce::jmin(numVisibleBlocks, request.safeMaxWidth);

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
    const double blocksPerTexel = (double) numVisibleBlocks / (double) textureWidthLocal;

    std::vector<juce::uint8> pixels((size_t) textureWidthLocal * 4);
    std::vector<juce::uint8> midPixels((size_t) textureWidthLocal * 4);

    if (blocksPerTexel <= 1.0)
    {
        // The common case (one texel per block): every texel's bytes come
        // straight from a cached tile - built once per (level, tile) the
        // first time it's needed, then just memcpy'd out on every later
        // rebuild that revisits it (panning back, zooming back out then in
        // near the same spot, etc.) instead of re-walking blocks and
        // re-quantising floats to bytes every single time.
        int t = 0;
        while (t < textureWidthLocal)
        {
            const int blockIndex = firstBlock + t;
            const int tileIndex = blockIndex / kTileBlocks;
            const int tileLocalStart = blockIndex - tileIndex * kTileBlocks;
            const auto& tile = getOrBuildTile(level, tileIndex, levelBlocks);

            const int blocksLeftInTile = tile.numBlocks - tileLocalStart;
            const int blocksLeftInWindow = textureWidthLocal - t;
            const int n = juce::jmax(0, juce::jmin(blocksLeftInTile, blocksLeftInWindow));
            if (n <= 0)
                break; // shouldn't happen, but avoid an infinite loop if it does

            std::memcpy(pixels.data() + (size_t) t * 4, tile.waveform.data() + (size_t) tileLocalStart * 4, (size_t) n * 4);
            std::memcpy(midPixels.data() + (size_t) t * 4, tile.mid.data() + (size_t) tileLocalStart * 4, (size_t) n * 4);

            t += n;
        }
    }
    else
    {
        // Rare fallback: even the coarsest mip level has more blocks in view
        // than the texture can hold, so several blocks must be reduced into
        // one texel. Not tile-cached (the grouping boundaries here depend on
        // the exact view, unlike the fixed tile grid) - this path is only
        // hit at extreme zoom-out on very long files.
        for (int t = 0; t < textureWidthLocal; ++t)
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
    }

    // The texture's bucket boundaries are snapped to whole fixed blocks
    // (floor/ceil above), so the sample range it actually covers is very
    // slightly wider than the precise [viewStart, viewStart+viewLength)
    // window - not the same thing. Record the texture's real span here so
    // the shader can map screen x to it exactly instead of assuming a 1:1
    // match; without this, that rounding offset changes as viewStart moves
    // continuously past each block boundary during a drag, which reads as
    // the waveform content subtly snapping/judder while panning.
    result.textureWidth = textureWidthLocal;
    result.pixels = std::move(pixels);
    result.midPixels = std::move(midPixels);
    result.textureViewStart = firstBlock * levelSamplesPerBlock;
    result.textureViewLength = (double) numVisibleBlocks * levelSamplesPerBlock;
    return result;
}

void WaveformComponent::applyBuildResult(BuildResult& result)
{
    // Written into the INACTIVE slot - the one not currently bound for
    // sampling by this frame's (about-to-happen) draw call - and only
    // flipped to active once the upload is done. See the member comment on
    // waveformTextures for why.
    const int backSlot = 1 - activeTextureSlot;

    // With the window fixed to targetWidth blocks (rather than wobbling
    // +/-1 with the exact view bounds), the width stays constant across
    // rebuilds almost all the time - so reuse the existing GPU storage with
    // a data-only update (glTexSubImage2D) instead of asking the driver to
    // reallocate it (glTexImage2D) on every single rebuild.
    glBindTexture(GL_TEXTURE_2D, waveformTextures[backSlot]);
    if (result.textureWidth == allocatedTextureWidths[backSlot])
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, result.textureWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE, result.pixels.data());
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, result.textureWidth, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, result.pixels.data());

    glBindTexture(GL_TEXTURE_2D, midTextures[backSlot]);
    if (result.textureWidth == allocatedTextureWidths[backSlot])
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, result.textureWidth, 1, GL_RGBA, GL_UNSIGNED_BYTE, result.midPixels.data());
    else
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, result.textureWidth, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, result.midPixels.data());

    allocatedTextureWidths[backSlot] = result.textureWidth;
    activeTextureSlot = backSlot;

    textureWidth = result.textureWidth;
    textureViewStart = result.textureViewStart;
    textureViewLength = result.textureViewLength;
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

    // SmoothPositionTracker's whole job is extrapolating a STEADY 1x forward
    // clock between infrequent updates - exactly wrong while scratching,
    // where the real motion is whatever speed/direction the mouse is
    // driving (including reverse), so its constant-forward-motion guess
    // would fight the actual movement and read as lag/rubber-banding.
    // AudioEngine::getPosition() already returns the scratch engine's own
    // continuously-updated position directly in that case, so it's used
    // as-is instead.
    const bool isScratchingNow = audioEngine.isScratching();
    const double smoothedSeconds = isScratchingNow
        ? audioEngine.getPosition()
        : playheadLineTracker.update(audioEngine.getPosition(), isPlayingNow);
    const double playheadSample = sampleRate > 0.0 ? smoothedSeconds * sampleRate : 0.0;

    // Amplitude pulse: the overall amplitude right at the playhead, run
    // through a fast-attack/slow-release envelope follower so a loud hit
    // reads as a quick visual punch that decays back down - not a flat,
    // static boost - and so the whole waveform visibly pulses in time with
    // the music while playing. Forced to 0 while not playing (isPlayingNow
    // already covers scratching too, via AudioEngine::isPlaying()), so the
    // pulse settles back to nothing rather than freezing mid-pulse the
    // instant playback stops.
    const float rawAmplitudeEnergy = isPlayingNow ? sampleAmplitudeAtSample(playheadSample) : 0.0f;
    const float amplitudePulseNow = amplitudePulseTracker.update(rawAmplitudeEnergy, isPlayingNow);

    if (followPlayhead && isPlayingNow && sampleRate > 0.0)
    {
        const juce::ScopedLock sl(dataLock);
        double newViewStart = playheadSample - viewLength * 0.5;

        // Snap to whole DEVICE-PIXEL steps in sample-space. Without this,
        // viewStart drifts by a continuous, sub-pixel amount every frame
        // (it's derived from playheadSample, which itself moves
        // continuously via SmoothPositionTracker) - so the sub-pixel PHASE
        // between the fixed block/texel grid and the screen's pixel grid is
        // never the same two frames in a row. Each pixel's block-boundary
        // blend (and the 2x horizontal supersample) is already correctly
        // anti-aliased for whatever that phase happens to be at any single
        // instant, but a continuously drifting phase makes each pixel's
        // blend weight oscillate over time even while the underlying audio
        // content on screen has barely changed - which is exactly a moire/
        // shimmer pattern, not a per-frame aliasing problem AA can fix.
        // Quantizing viewStart to whole pixels holds that phase fixed
        // between two consecutive whole-pixel steps, so it stops drifting;
        // motion still reads as smooth since a single device pixel is well
        // below the threshold of visible "stepping".
        const double devicePixelWidth = juce::jmax(1, getWidth()) * openGLContext.getRenderingScale();
        const double samplesPerDevicePixel = viewLength / juce::jmax(1.0, devicePixelWidth);
        if (samplesPerDevicePixel > 0.0)
            newViewStart = std::round(newViewStart / samplesPerDevicePixel) * samplesPerDevicePixel;

        viewStart = newViewStart;
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
    // strictly needs (see buildTextureData), so most of the time the new
    // view is still fully covered by whatever's already uploaded - only
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
    {
        // Hand the rebuild off to the background thread instead of doing the
        // CPU packing inline here - this frame keeps drawing with whatever
        // texture is already active while the worker catches up, rather
        // than stalling however long the packing takes. See the class
        // comment for the resulting lag-instead-of-stutter tradeoff.
        const int pixelWidth = juce::jmax(1, (int) std::ceil((double) getWidth() * openGLContext.getRenderingScale()));
        const int safeMaxWidth = safeMaxTextureWidth.load();
        const int paddedWidth = (int) std::ceil((double) pixelWidth * kTextureWindowPadding);

        BuildRequest request;
        request.viewStart = localViewStart;
        request.viewLength = localViewLength;
        request.targetWidth = juce::jmin(paddedWidth, safeMaxWidth);
        request.safeMaxWidth = safeMaxWidth;
        request.generation = nextGeneration.fetch_add(1, std::memory_order_relaxed);

        {
            const juce::ScopedLock sl(requestLock);
            pendingRequest = request;
            hasPendingRequest = true;
        }
        buildRequestEvent.signal();

        textureDirty = false; // the just-posted request now owns getting this rebuilt
    }

    // Pick up the latest finished build, if any, and upload it - the only
    // part of a rebuild that has to run on the GL thread.
    {
        BuildResult completed;
        bool haveCompleted = false;
        {
            const juce::ScopedLock sl(resultLock);
            if (hasReadyResult && readyResult.generation != displayedGeneration)
            {
                completed = std::move(readyResult);
                haveCompleted = true;
                hasReadyResult = false;
            }
        }

        if (haveCompleted)
        {
            applyBuildResult(completed);
            displayedGeneration = completed.generation;
        }
    }

    if (shader == nullptr || textureWidth == 0)
        return;

    const double total = getTotalLength();
    if (total <= 0.0)
        return;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    shader->use();

    // Always sample whichever slot uploadWaveformTexture last flipped to -
    // never the one it might currently be writing into.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, waveformTextures[activeTextureSlot]);
    shader->setUniform("waveformTex", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, midTextures[activeTextureSlot]);
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
    setColour("amplitudeColour", params.amplitudeColour);
    shader->setUniform("lowAmount", params.lowFreqAmount);
    shader->setUniform("midAmount", params.midFreqAmount);
    shader->setUniform("highAmount", params.highFreqAmount);
    shader->setUniform("midPole", params.midPole);
    shader->setUniform("waveformHeight", params.waveformHeight);
    shader->setUniform("amplitudeAmount", params.amplitudeAmount);
    shader->setUniform("amplitudeColorAmount", params.amplitudeColorAmount);
    shader->setUniform("amplitudeGlowRadius", params.amplitudeGlowRadius);
    shader->setUniform("amplitudePulse", amplitudePulseNow);
    shader->setUniform("amplitudeRangeNorm", params.amplitudeRange);
    shader->setUniform("amplitudeSlope", juce::jmax(0.001f, params.amplitudeSlope));

    {
        float lowWeight, midWeight, highWeight;
        computeAmplitudeBandWeights(params.amplitudeMinFrequencyHz, params.amplitudeMaxFrequencyHz, lowWeight, midWeight, highWeight);
        shader->setUniform("amplitudeBandWeights", lowWeight, midWeight, highWeight);
    }

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
    glDeleteTextures(2, waveformTextures);
    glDeleteTextures(2, midTextures);
    shader.reset();
}
