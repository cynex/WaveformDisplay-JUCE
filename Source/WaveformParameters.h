#pragma once

#include <JuceHeader.h>

/** All the tweakable knobs that control how the waveform is rendered.
    Passed by value/reference into WaveformComponent and mirrored by
    the ParametersPanel UI. */
struct WaveformParameters
{
    // Base colour of the waveform. The low/mid/high colours act as
    // multipliers on top of this colour, weighted by the amount of
    // low-frequency / mid-frequency / high-frequency energy present in each
    // analysed block.
    juce::Colour solidColour   { juce::Colours::white };
    juce::Colour lowFreqColour { juce::Colour(0xff0007ff) };
    juce::Colour midFreqColour { juce::Colour(0xffff00ff) };
    juce::Colour highFreqColour{ juce::Colour(0xffffcc00) };

    // Two independent additive amplitude glow colours - added (not mixed/
    // multiplied) on top of the low/mid/high tint, so the waveform visibly
    // brightens/glows in these colours in time with the music, independently
    // of whatever the frequency tinting is currently doing (including with
    // tinting disabled entirely). amplitudeLowColour is driven purely by
    // each block's LOW-frequency energy, amplitudeHighColour purely by its
    // HIGH-frequency energy - each with its OWN independent envelope-
    // followed pulse (see AmplitudePulseTracker / the amplitudeLowPulse and
    // amplitudeHighPulse uniforms), so a bass hit and a treble hit each
    // produce their own colour's punch at their own moment, rather than
    // both being tied to one shared broadband reading. Strength (shared
    // between the two) is controlled by amplitudeColorAmount below. See
    // coverageFor/sampleWaveform in the shader.
    juce::Colour amplitudeLowColour { juce::Colours::white };
    juce::Colour amplitudeHighColour{ juce::Colour(0xffff6a00) };

    // How strongly the low/mid/high colours are allowed to tint the solid colour.
    float lowFreqAmount  = 1.0f;
    float midFreqAmount  = 1.0f;
    float highFreqAmount = 1.0f;

    // Nominal height scale for the waveform's rendered envelope, applied
    // BEFORE the amplitude-driven height boost below - i.e. this shrinks
    // (or grows) the waveform's baseline size first, and the boost inflates
    // on top of whatever that baseline turns out to be. Deliberately
    // separate from the boost itself: a block already sitting near its full
    // +/-1 rendered height has nowhere left to grow when boosted, so the
    // boost effect reads as "crushed"/invisible right at the loudest
    // moments, which is exactly the opposite of what it's for. Turning this
    // DOWN gives the boost real headroom to visibly inflate blocks without
    // immediately hitting that ceiling, so the two together read as a
    // waveform that's normally more modest in height but "pops" out to (or
    // past) full size on a hit - a balanced pulse rather than a maxed-out,
    // clipped-looking one. 1.0 is the original, unscaled behaviour.
    float waveformHeight = 0.7f;

    // 0..1 mix towards a fixed, deliberately exaggerated amplitude effect
    // that visibly inflates a block's rendered height based on how LOUD it
    // currently is (its own energy in the frequency range selected by
    // amplitudeMinFrequencyHz/amplitudeMaxFrequencyHz below - the full
    // spectrum by default) AND (see amplitudeRange below) how close it is
    // to the playhead. The whole waveform also visibly PULSES in time with the
    // music while playing: WaveformComponent continuously samples the
    // amplitude right at the playhead and runs it through a fast-attack/
    // slow-release envelope follower (see AmplitudePulseTracker), so a loud
    // hit produces a quick punch that decays back down, rather than a
    // static per-block bulge. 0 is an exact no-op (the waveform's
    // silhouette renders exactly as analysed, regardless of how loud the
    // audio is); 1 applies the effect at full strength. On top of (not
    // instead of) the low/mid/high colour tinting - deliberately NOT trying
    // to be an accurate amplitude representation ITSELF, the point is to
    // make a beat visibly "punch" through the waveform's shape, not just
    // its colour, in a fun/over-emphasized way. See coverageFor in the
    // shader for the actual curve. Only affects rendered HEIGHT - see
    // amplitudeColorAmount below for the separate colour-glow strength.
    float amplitudeAmount = 0.5f;

    // Strength of the two additive amplitudeLowColour/amplitudeHighColour
    // glows (see above) - a SEPARATE control from amplitudeAmount, so the
    // colour glows can be pushed harder (or softer) than the height boost
    // independently. Shared between both colours (their individual pulses
    // still differ). Goes up to 4 (not clamped to 1 like amplitudeAmount)
    // since it's driving a purely additive light effect with no "overflow"
    // concern the way the height boost has (which has to stay inside the
    // component's +/-1 amplitude range) - values above 1 just make the
    // glow read as brighter/more saturated at the loudest moments. Rides
    // the same range mask as amplitudeAmount, so all three still pulse
    // together spatially even at different magnitudes and different
    // (per-band) timing.
    float amplitudeColorAmount = 1.0f;

    // Radius (pixels) of an additive glow halo that floods the NEGATIVE
    // SPACE around the waveform's own silhouette - a distinct effect from
    // amplitudeColorAmount's interior colour glow above, though both use
    // amplitudeLowColour/amplitudeHighColour and are driven by the exact
    // same amplitude-pulse "gain" (see coverageFor's haloOut, including its
    // own extra intensity boost so it actually reads as visible ambient
    // glow rather than a faint edge fringe), so the halo brightens/dims in
    // lockstep with everything else the amplitude effect is doing. 0
    // disables the halo
    // entirely (falls off to nothing at the silhouette's own edge); larger
    // values let it spread further into the background before fading out -
    // the default is deliberately generous (a large fraction of a typical
    // component's height) so it reads as the waveform's own presence
    // filling the space around it, not a thin outline.
    float amplitudeGlowRadius = 60.0f;

    // 0..1 fraction of the current view width over which the amplitude
    // effect above is allowed to apply, centred on and fading out from the
    // playhead - falls off LOGARITHMICALLY with distance (see
    // sampleWaveform's rangeCoverage), so the effect stays strong for a
    // good stretch right around the playhead and then tails off, rather
    // than fading linearly - reads as a distinct "pulse hugging the
    // playhead" instead of a diffuse wash across the visible waveform.
    // Small values isolate it to a narrow band right at the playhead;
    // larger values (still capped well under the full view width, so it
    // never washes out into a uniform effect) reach further either side.
    // Purely a spatial mask; doesn't change amplitudeAmount's or
    // amplitudeColorAmount's own strength, just where on screen it's
    // allowed to show.
    float amplitudeRange = 0.05f;

    // Gamma exponent (x^(1/amplitudeSlope)) the amplitude effect's driving
    // value is warped through before it's used for the height boost/colour
    // glow (see coverageFor's ampCurve call) - HIGHER values lift QUIET
    // input much more aggressively (x=0.01 already maps to ~0.32 at
    // amplitudeSlope=4, versus barely moving at amplitudeSlope=1), so even
    // subtle/quiet audio produces a clearly visible pulse instead of
    // needing to already be fairly loud before the effect shows any
    // action - while a genuine hit (x==1) always still maps to exactly 1,
    // so it never overshoots. 1.0 is the identity (no reshaping at all).
    // Purely reshapes how the AMPLITUDE value maps to effect strength -
    // independent of amplitudeRange's own (separately, fixed-steepness)
    // distance-from-playhead falloff curve, which this does not affect.
    float amplitudeSlope = 4.0f;

    // Frequency range (Hz) the amplitude effect's driving value is focused
    // on, e.g. narrowing to roughly the mid band so the height boost/colour
    // glow track mid-range content (vocals, snare, etc.) rather than
    // reacting to sub-bass or cymbals too. Approximated cheaply from the
    // THREE bands AudioEngine already analyses (low/mid/high, split at the
    // fixed 300Hz/3000Hz points used for the low/mid/high colour tinting)
    // rather than a true band-pass reanalysis - WaveformComponent blends
    // those three bands' energy in proportion to how much of
    // [amplitudeMinFrequencyHz, amplitudeMaxFrequencyHz] overlaps each
    // one, and that blended value (not the broadband peak amplitude) is
    // what drives the boost/glow. This means the achievable selectivity is
    // limited to what those two fixed split points allow - e.g. "mostly
    // mid" is straightforward, but an arbitrary narrow band inside the mid
    // range isn't distinguishable from the rest of it. The full default
    // range (20Hz-20000Hz) covers all three bands evenly, closest to (but
    // not identical to) the previous broadband-peak-driven behaviour.
    float amplitudeMinFrequencyHz = 20.0f;
    float amplitudeMaxFrequencyHz = 20000.0f;

    // Purely a rendering/blend control, NOT audio analysis - the mid-band
    // energy value itself (computed once at file load, fixed 300Hz/3000Hz
    // split) is untouched. This just warps how strongly that value reads in
    // the low/mid/high colour blend: 0.5 (default) leaves it unchanged: an
    // input mid-energy of 0.5 still blends at 0.5. Moving it towards 0 makes
    // moderate mid-energy blocks read as MORE strongly mid-tinted (an input
    // of `midPole` now blends at 0.5, so lower inputs reach that pivot
    // sooner); moving it towards 1 does the opposite, requiring a stronger
    // mid-energy block before the mid tint reads as dominant. Applied in the
    // shader as a gamma curve pivoting at this point - see sampleWaveform.
    float midPole = 0.5f;

    // Master switch for the low/mid/high frequency tinting above - when
    // false the waveform renders as a flat solidColour regardless of the
    // amount sliders.
    bool tintingEnabled = true;

    // Signed-distance-field shaping.
    // aaWidth   - width (in pixels) of the anti-aliased edge transition.
    // smoothing - additional feathering applied on top of the AA edge,
    //             used to soften the waveform silhouette (e.g. when zoomed
    //             far out and many samples are collapsed per pixel).
    float aaWidth    = 1.5f;
    float smoothing  = 0.0f;

    // Background colour behind the waveform.
    juce::Colour backgroundColour{ juce::Colours::black };

    // Colour of the playback position indicator.
    juce::Colour playheadColour{ juce::Colours::red };

    // Opacity (0..1) of a solid white 1px horizontal line drawn through the
    // vertical centre of the waveform (the zero-amplitude axis). 0 hides it entirely.
    float centreLineAlpha = 1.0f;
};
