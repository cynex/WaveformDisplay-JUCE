# WaveformDisplay

Standalone Windows JUCE app that loads a WAV/FLAC/OGG/MP3 file and renders
its waveform using a signed-distance-field OpenGL shader.

## Features

- File open dialog + drag-and-drop, playback via the default audio device.
- GPU waveform rendering (`Source/WaveformComponent.cpp`): the full analysis
  buffer (min/max envelope + low/high band energy per block, computed in
  `AudioEngine::analyse`) is uploaded once as a 1D float texture; the
  fragment shader reconstructs a signed distance field per pixel each frame,
  so zooming stays crisp at any level.
- Mouse wheel zooms (centred on the cursor), click-drag pans.
- Scrollbar below the waveform: handle position = current scroll offset,
  handle width = current zoom (view length / total length).
- Right-hand panel: solid colour, low-frequency tint colour, high-frequency
  tint colour (both multiplicative against the solid colour, weighted by a
  0..1 "amount" slider), SDF anti-alias width, and an extra smoothing/feather
  control.

## Building

Requires CMake 3.22+ and a JUCE checkout (either as `../JUCE` next to this
folder, fetched automatically via `FetchContent`, or pointed to explicitly):

```
cmake -B build -DJUCE_PATH=C:/path/to/JUCE   # or omit -DJUCE_PATH to fetch JUCE from git
cmake --build build --config Release
```

The built executable will be under `build/WaveformDisplay_artefacts/Release/`.

## Notes

- MP3 decoding relies on JUCE's built-in `MP3AudioFormat` (decode-only,
  registered via `AudioFormatManager::registerBasicFormats()`), available in
  JUCE 6.1+.
- `AudioEngine` splits energy into low/mid/high bands with cascaded one-pole
  filters (cutoffs ~300 Hz / ~3 kHz) purely to drive the waveform's colour
  tinting — it is not an audio-quality analysis tool.

## Development history: chasing flicker and jutter

By the time the waveform was rendering the right shape at the right
resolution, it still had a long-running problem: flicker and jutter, in
several unrelated flavours, that took many rounds to fully resolve. This
section exists so that if something in this area breaks again, whoever's
looking at it doesn't have to rediscover all of this from scratch. The short
version: almost every one of these bugs looked like it could be a dozen
different things, and the ones that actually got fixed on the first guess
were the exception, not the rule. The turning point on the hardest bug was
abandoning theory entirely and adding temporary instrumentation to log what
was actually happening.

### The problems, and what actually explained them

Each fix below solved a real, verified bug — but each one also turned out to
be necessary and not sufficient, because there were multiple independent
bugs producing symptoms that all just looked like "flickering":

1. *Sparse peak sampling.* An early version of the fragment shader handled
   zoomed-out views by sampling a capped number of "taps" (max 64) across
   however many texels a screen pixel covered. Once a pixel covered more
   than 64 blocks, this was no longer a true min/max — it was a sample —
   and *which* blocks got sampled shifted as the view moved by sub-pixel
   amounts, causing peaks to visibly pop in and out. Fixed by making the
   mip pyramid do the reduction exhaustively instead (every level's texel
   already covers 100% of the samples under it, not a sample of them).

2. *Texture-to-view rounding mismatch ("panning judder").* The texture's
   bucket boundaries are snapped to whole fixed blocks, so the sample range
   it actually spans is very slightly wider than the exact visible window.
   Mapping screen-x directly onto the texture (assuming a 1:1 match) meant
   that rounding offset changed continuously as the view panned across
   block boundaries, reading as a subtle snapping/judder. Fixed by tracking
   the texture's *actual* covered range and correcting for it explicitly
   (`texMapOffset`/`texMapScale`) rather than assuming an exact match.

3. *Peak "breathing" from linear texture filtering.* `GL_LINEAR` filtering
   (added to smooth blockiness when zoomed in) was smoothly interpolating
   the min/max *values* between adjacent blocks — but there's no such thing
   as "70% of the way between these two peaks" in real audio. A tall,
   one-block-wide peak would visibly shrink and grow as the sub-pixel
   alignment between it and the screen changed. Fixed by switching to
   `GL_NEAREST` (each block keeps its true, un-blended height) and doing
   proper edge anti-aliasing a different way: the shader looks up both
   neighbouring blocks explicitly and cross-fades the SDF *coverage*
   between them (i.e. anti-aliases the shape's edge, not its height).

4. *The same bug, self-inflicted, during the coverage-crossfade fix above.*
   The boundary crossfade's blend width was accidentally tied to the
   `AA Width` parameter. At the default width, this made the crossfade span
   *wider than the gap between texel centres* — meaning almost every pixel
   was constantly blending between two different blocks' heights again,
   reintroducing bug #3 by a different mechanism. This is also why it
   started showing up during plain panning, not just playhead-follow: it
   wasn't tied to any particular kind of motion, it was baked into the
   per-pixel math. Fixed by decoupling the boundary blend width from
   `AA Width` entirely (fixed at roughly half a screen pixel).

5. *Smoothing exaggerating quiet blocks.* The `Smoothing` parameter widened
   the SDF's vertical edge feather by a fixed screen-space amount,
   independent of each block's own height. A feather several times taller
   than a quiet block's actual amplitude made it look disproportionately
   hazy next to a loud neighbour, and that mismatch changing block-to-block
   as the view scrolled read as flicker that visibly got worse the higher
   `Smoothing` was pushed. Fixed by scaling the extra feather by each
   block's own half-height instead of a fixed amount.

6. *A real data race.* `setViewRange` (called from the message thread by
   mouse handling and, at the time, a follow-playhead timer) mutated the
   view's sample range with no synchronisation against `renderOpenGL`
   reading the same fields on JUCE's separate GL thread. A single stray
   read was rare enough during sporadic manual panning to go unnoticed, but
   once follow-playhead started calling `setViewRange` continuously, the
   same race was hit on nearly every frame. Fixed with a `CriticalSection`
   and by taking one consistent snapshot of the view at the top of each
   render, instead of re-reading the live, concurrently-mutating fields
   partway through.

7. *Frame pacing.* Rendering was driven by a CPU `Timer` calling
   `triggerRepaint()`, which isn't synced to the display's actual vsync.
   Switched to `openGLContext.setContinuousRepainting(true)` (plus an
   explicit `setSwapInterval(1)`) so the GL thread's own render loop is
   paced by the driver instead.

8. *The big one: playhead jitter that got worse the more you zoomed in.*
   This looked at first like the audio position itself was unstable, and
   several rounds of fixing were spent on that theory (see the log below).
   None of them helped, because the actual position was fine. The real
   cause: the view was recentred on the playhead from a 60Hz timer, while
   the drawn line was recomputed fresh every render frame from a
   more-up-to-date reading. Between two timer ticks, the line legitimately
   drifted away from centre as playback advanced, then visibly snapped back
   the instant the next tick recentred the view. That drift is a *fixed
   number of samples* per tick, so it became a larger and larger fraction
   of the visible window — and so more visible — the further you zoomed
   in, which is exactly the reported symptom. This is what finally got
   found by adding temporary logging of the raw position stream instead of
   continuing to guess (see below). Fixed by computing the view-centring
   and the drawn line from the *identical* reading inside `renderOpenGL`,
   so there's no time gap left for anything to drift across.

9. *Texture reallocation stalls.* The texture's width was derived from
   `floor`/`ceil` of the exact view bounds, which wobbles by ±1 texel
   almost every rebuild as the view shifts continuously. Every width change
   forces the GL driver to fully reallocate the texture (`glTexImage2D`)
   rather than just update its contents — an occasional reallocation stall
   landing on the wrong frame is a very plausible source of "very minor,
   intermittent" flicker that's otherwise hard to pin down. Fixed by fixing
   the texture's block window to a constant width wherever there's enough
   file on both sides to allow it, and using `glTexSubImage2D` (data-only
   update) instead of `glTexImage2D` (reallocation) whenever the width
   hasn't actually changed.

10. *Rebuilding the texture every single frame.* Once follow-playhead moved
    into `renderOpenGL` (fix #8), it also — unintentionally — started
    marking the texture dirty on every frame instead of every 60Hz tick,
    meaning a full CPU aggregation + GPU upload on every vsync (up to
    ~144/sec). The texture window is deliberately much wider than any one
    frame's view needs, so almost none of those rebuilds were necessary.
    Fixed by only marking the texture dirty once the view is actually about
    to slide outside the range that's already loaded.

### What was tried for the playhead-jitter bug specifically, in order, and why most of it didn't help

This particular bug (#8 above) is worth calling out on its own, because it's
the clearest example in this project of chasing plausible-sounding theories
before actually measuring anything — five real fixes landed along the way
(each fixing a genuine, separate bug above), and none of them touched the
actual root cause until the position data was logged directly.

| # | Theory | Change made | Result |
|---|--------|--------------|--------|
| 1 | Follow-playhead recentring and the drawn line just needed to be smoother | Extrapolated the playback position continuously between `AudioEngine::getPosition()` updates using wall-clock time | Reduced *some* real jutter (see fix #6/#7 above, which were genuine bugs found along the way) but the core zoomed-in jump remained |
| 2 | The position reports were mildly non-monotonic (resampler read-ahead jitter) | Added a monotonic floor that suppressed small backward moves | No observable change — turned out the position was already clean |
| 3 | The extrapolator's periodic drift-correction snaps were themselves the problem | Removed the extrapolator entirely, read `getPosition()` directly every frame | No observable change |
| 4 | It must be vsync/frame-pacing related, since the jump scaled with zoom | Re-checked/confirmed `setSwapInterval(1)` and continuous repainting (already in place from fix #7) | No observable change |
| 5 | Maybe the texture rebuild itself was introducing timing noise | Made the texture window a fixed width and switched to `glTexSubImage2D` (this *is* fix #9 above, and worth doing regardless) | Fixed a different, real problem; zoomed-in jump still present |
| — | Stopped guessing; added temporary logging of `(wall-clock time, raw position, smoothed position, view length)` to a file during playback | — | The logged data showed the raw position was **already smooth, monotonic, and updating every ~16.7ms** — every theory above was chasing a problem that didn't exist |
| 6 | With the position ruled out, the only remaining variable was *when* the view gets recentred vs. *when* the line gets drawn | Moved follow-playhead's view-centring into `renderOpenGL`, computed from the same reading used to draw the line, in the same call | Fixed — this was the actual bug |

The lesson that generalises: once a couple of plausible-sounding fixes in a
row produce no visible change, it's a sign the mental model of the bug is
wrong, not that the fix needs to be more aggressive. Instrumenting and
reading the real data directly settled in one step what several rounds of
reasoning about JUCE/resampler internals couldn't.
