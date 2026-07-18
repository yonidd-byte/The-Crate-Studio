#include "CrateMixerLookAndFeel.h"
#include "BinaryData.h"

#include <map>

namespace
{
    struct DbTick { double db; const char* label; };

    // Pan knob filmstrip — 49 vertical frames, one already-rendered rotation
    // per frame. Loaded once (ImageCache::getFromMemory already hashes/caches
    // internally, but a function-local static avoids paying even that lookup
    // cost every paint() call). No fallback shape is drawn if this fails to
    // load — a missing/misconfigured binary asset should be visibly obvious
    // (an invisible knob) rather than papered over with a silent substitute.
    const juce::Image& getPanKnobFilmstrip()
    {
        static const juce::Image img = juce::ImageCache::getFromMemory (BinaryData::pan_knob_png, BinaryData::pan_knob_pngSize);
        return img;
    }

    // QA Hardening fix — drawRotarySlider used to call g.drawImage() with a
    // source frame size (e.g. 72x72) different from the destination size
    // (e.g. ~40px for the main Pan knob, ~14px for a Send mini-knob), which
    // forces a bilinear RESCALE on every single paint() call — including
    // every frame of a drag. This cache rescales the ENTIRE filmstrip to a
    // given on-screen frame size exactly ONCE (keyed by that size — there are
    // only ever a handful of distinct pan-knob sizes on screen at once, so
    // this stays tiny and never grows unbounded), so every subsequent paint()
    // just blits one ALREADY-the-right-size frame 1:1 — no per-frame resample.
    const juce::Image& getScaledPanKnobFilmstrip (int destSize)
    {
        static std::map<int, juce::Image> cache;
        static juce::Image invalid;

        if (destSize <= 0)
            return invalid;

        if (auto it = cache.find (destSize); it != cache.end())
            return it->second;

        const auto& source = getPanKnobFilmstrip();
        if (! source.isValid())
            return invalid;

        const int srcW = source.getWidth();
        const int numFrames = source.getHeight() / srcW;
        if (numFrames <= 0)
            return invalid;

        juce::Image scaled (juce::Image::ARGB, destSize, destSize * numFrames, true);
        {
            juce::Graphics g (scaled);
            g.drawImage (source, 0, 0, destSize, destSize * numFrames, 0, 0, srcW, source.getHeight());
        }

        return cache.emplace (destSize, std::move (scaled)).first->second;
    }

    // Asset-based fader cap (V2.0 Manifesto pivot: hybrid rendering — procedural
    // groove/ticks for dynamic scaling, a photorealistic PNG for the thumb).
    // fader_cap.png bakes its OWN drop shadow into the bottom/right of the
    // image, so drawing it 1:1 would misalign the visible metal body against
    // the groove/sliderPos. bodyBounds is the image's OPAQUE region (the real
    // cap, shadow excluded) — scanned from the actual pixel alpha ONCE (cached
    // in this function-local static) rather than hardcoded margins that would
    // silently go stale if the asset is ever replaced.
    // image/bodyBounds are the FINAL, already-integer-scaled assets — resized
    // ONCE (below) to their exact on-screen size so paint() never rescales a
    // float rect per frame. Rescaling a sharp-ridged PNG at a different
    // fractional scale/position every single drag frame is exactly what was
    // causing the sub-pixel jitter/shimmer: the destination size and the
    // source pixels must line up 1:1, with only whole-pixel (int) translation
    // varying frame to frame.
    struct FaderCapAsset
    {
        juce::Image image;                  // pre-scaled to its final on-screen size
        juce::Rectangle<int> bodyBounds;     // opaque (shadow-excluded) region, in THIS (scaled) image's pixel coords
        bool valid = false;
    };

    const FaderCapAsset& getFaderCapAsset()
    {
        static const FaderCapAsset asset = []
        {
            FaderCapAsset a;
            auto raw = juce::ImageCache::getFromMemory (BinaryData::fader_cap_png, BinaryData::fader_cap_pngSize);

            if (! raw.isValid())
                return a;

            // One-time scan of the RAW asset for the bounding box of near-opaque
            // pixels — soft shadow pixels fall well below this alpha threshold,
            // so they're excluded from the body region.
            constexpr int opaqueAlphaThreshold = 200;
            const int rawW = raw.getWidth();
            const int rawH = raw.getHeight();
            int minX = rawW, maxX = -1, minY = rawH, maxY = -1;

            {
                juce::Image::BitmapData bitmap (raw, juce::Image::BitmapData::readOnly);

                for (int y = 0; y < rawH; ++y)
                {
                    for (int x = 0; x < rawW; ++x)
                    {
                        if (bitmap.getPixelColour (x, y).getAlpha() >= opaqueAlphaThreshold)
                        {
                            minX = juce::jmin (minX, x); maxX = juce::jmax (maxX, x);
                            minY = juce::jmin (minY, y); maxY = juce::jmax (maxY, y);
                        }
                    }
                }
            }

            const juce::Rectangle<int> rawBodyBounds =
                (maxX >= minX && maxY >= minY) ? juce::Rectangle<int> (minX, minY, maxX - minX + 1, maxY - minY + 1)
                                                : raw.getBounds();

            // Target ON-SCREEN size of the BODY (shadow excluded) — matches the
            // old procedural cap's footprint (28x40) so groove/tick layout is
            // unaffected. Scale + integer-round the WHOLE image and body bounds
            // together, ONCE, here — not per paint() call.
            constexpr float desiredBodyW = 28.0f;
            constexpr float desiredBodyH = 40.0f;
            const float scale = juce::jmin (desiredBodyW / (float) rawBodyBounds.getWidth(),
                                             desiredBodyH / (float) rawBodyBounds.getHeight());

            const int scaledImgW = juce::jmax (1, juce::roundToInt ((float) rawW * scale));
            const int scaledImgH = juce::jmax (1, juce::roundToInt ((float) rawH * scale));

            a.image = raw.rescaled (scaledImgW, scaledImgH, juce::Graphics::highResamplingQuality);
            a.bodyBounds = juce::Rectangle<int> (juce::roundToInt ((float) rawBodyBounds.getX() * scale),
                                                 juce::roundToInt ((float) rawBodyBounds.getY() * scale),
                                                 juce::roundToInt ((float) rawBodyBounds.getWidth()  * scale),
                                                 juce::roundToInt ((float) rawBodyBounds.getHeight() * scale));
            a.valid = a.image.isValid();
            return a;
        }();

        return asset;
    }
}

CrateMixerLookAndFeel::CrateMixerLookAndFeel() = default;

//==============================================================================
// The photorealistic Waves LV1-style VERTICAL fader. Every layer is drawn
// procedurally (gradients + hand-placed lines) — no juce::DropShadow/component
// effects, so the whole cap composites in one pass with no clipping surprises.
void CrateMixerLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPos, float minSliderPos, float maxSliderPos,
                                               juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        TheCrateLookAndFeel::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    constexpr float faderColumnWidth = 34.0f;
    const bool hasRoomForLabels = (float) width > 46.0f;
    const float centreX = (float) x + faderColumnWidth * 0.5f;

    // ---- The groove: 6px pill, HORIZONTAL gradient for a cylindrical hole -----
    // Left #000000 (deep shadow) -> Right #303038 (light catch): the horizontal
    // shading is what reads as a round hole carved into the panel, not a flat
    // painted slot.
    constexpr float grooveWidth = 6.0f;
    const juce::Rectangle<float> groove (centreX - grooveWidth * 0.5f, (float) y, grooveWidth, (float) height);
    juce::ColourGradient grooveGrad (juce::Colours::black, groove.getX(),     groove.getCentreY(),
                                      CrateColors::LightBackground, groove.getRight(), groove.getCentreY(), false);
    g.setGradientFill (grooveGrad);
    g.fillRoundedRectangle (groove, grooveWidth * 0.5f); // radius == half width -> perfect pill

    // ---- Console scale: major + minor ticks, numbers beside the majors -------
    const auto rangeMin = slider.getMinimum();
    const auto rangeMax = slider.getMaximum();
    const auto rangeSpan = rangeMax - rangeMin;

    auto yForDb = [&] (double db) -> float
    {
        const double clamped = juce::jlimit (rangeMin, rangeMax, db);
        const double proportion = rangeSpan > 0.0 ? (clamped - rangeMin) / rangeSpan : 0.5;
        return (float) y + (float) height * (float) (1.0 - proportion);
    };

    // Ticks live to the LEFT of the groove (engraved faceplate scale).
    const float tickRightX = groove.getX() - 2.0f;

    auto drawTick = [&] (double db, bool major)
    {
        const float ty = yForDb (db);
        const float leftX = major ? ((float) x + 1.0f) : (tickRightX - 6.0f);
        const float thick = major ? 2.0f : 1.0f;
        g.setColour (major ? CrateColors::BrandGray : CrateColors::BrandGray.withAlpha (0.55f)); // minor ticks dimmed, not a second grey literal
        g.fillRect (juce::Rectangle<float> (leftX, ty - thick * 0.5f, juce::jmax (1.0f, tickRightX - leftX), thick));
    };

    // Minor ticks every 2 dB across the whole range.
    for (double db = rangeMin; db <= rangeMax + 0.001; db += 2.0)
        drawTick (db, false);

    const DbTick majors[] = { { 6.0, "+6" }, { 0.0, "0" }, { -10.0, "-10" }, { -20.0, "-20" }, { rangeMin, "-INF" } };

    g.setFont (juce::FontOptions (7.0f));

    for (auto& t : majors)
    {
        drawTick (t.db, true);

        if (hasRoomForLabels)
        {
            const auto labelX = groove.getRight() + 3.0f;
            const auto labelWidth = juce::jmax (0.0f, (float) (x + width) - labelX);
            g.setColour (TheCrateLookAndFeel::colorTextSecondary); // #8E8E93
            g.drawText (t.label, juce::Rectangle<float> (labelX, yForDb (t.db) - 5.0f, labelWidth, 10.0f),
                        juce::Justification::centredLeft);
        }
    }

    // ---- Premium neon LED trail (FL Studio-style level feedback) --------------
    // Focus-driven UI (Manifesto's Anti-Fatigue Rule: the accent is a spotlight
    // reserved for 2-5% of pixels, not a permanent fixture) — ONLY drawn while
    // the user is actually touching this fader, so 40 simultaneous tracks don't
    // all glow at once. The dark base track/groove above stays unconditional.
    // Drawn AFTER the base track/groove, BEFORE the thumb — rises from the
    // bottom of the slot up to the thumb's exact vertical centre, so it reads
    // as "how much signal is below this point" the same way FL's fader trail
    // does. Needs the cap's thumb height to find that centre, so the asset is
    // fetched here (once) and reused, unchanged, by the thumb-drawing block
    // below — this does NOT touch or reorder that block's own math.
    if (slider.isMouseOverOrDragging())
    {
        const auto& capForTrail = getFaderCapAsset();
        const float thumbHeight = capForTrail.valid ? (float) capForTrail.bodyBounds.getHeight() : 40.0f;

        const float trailTop    = sliderPos + thumbHeight * 0.5f;
        const float trailBottom = (float) (y + height);

        if (trailBottom > trailTop)
        {
            constexpr float trailWidth = 3.0f; // narrower than the 6px groove — reads as an inset LED strip
            const juce::Rectangle<float> trail (centreX - trailWidth * 0.5f, trailTop, trailWidth, trailBottom - trailTop);

            juce::ColourGradient trailGrad (CrateColors::NeonBlue.withAlpha (0.8f), centreX, trailTop,
                                             CrateColors::NeonBlue.darker (0.7f), centreX, trailBottom, false);
            g.setGradientFill (trailGrad);
            g.fillRoundedRectangle (trail, trailWidth * 0.5f);
        }
    }

    // ---- The fader cap: photorealistic PNG asset ------------------------------
    // Hybrid rendering pivot — groove/ticks above stay procedural (need to
    // rescale with the slider's actual height), the cap is now the real
    // fader_cap.png asset for 100% photorealism at zero per-frame draw cost
    // beyond a single image blit.
    //
    // SUB-PIXEL RENDERING: destination position is kept as exact float — no
    // roundToInt/cast to int on the Y placement. sliderPos (from
    // Slider::getPositionOfProportionalValue(), passed in by JUCE already as a
    // float) carries genuine fractional precision during a drag; snapping it
    // to whole pixels is what produced the visible "stepping" — the cap
    // visibly jumping 1px at a time instead of gliding. Using the
    // Rectangle<float> drawImage() overload lets the graphics backend
    // (CoreGraphics/Direct2D) anti-alias/interpolate the edge at the true
    // fractional position, trading a hair of edge softness for genuinely
    // smooth 60FPS motion — the image is still pre-scaled to its final integer
    // SIZE once in getFaderCapAsset(), so this only re-introduces sub-pixel
    // translation, never a per-frame rescale.
    const auto& cap = getFaderCapAsset();

    if (cap.valid)
    {
        // Body centre (NOT the raw image's centre, which extends further
        // right/down for the baked-in shadow) must land on (centreX, sliderPos)
        // — the fader track's X-axis and the current value position.
        const float destX = centreX - (float) cap.bodyBounds.getCentreX();
        const float destY = sliderPos - (float) cap.bodyBounds.getCentreY();

        g.drawImage (cap.image, juce::Rectangle<float> (destX, destY,
                                                         (float) cap.image.getWidth(), (float) cap.image.getHeight()));

        // Live "on contact" glow, layered ON TOP of the image's own baked-in
        // white centre line rather than replacing it — same float sliderPos,
        // so the glow tracks the cap with zero sub-pixel drift between them.
        if (slider.isMouseOverOrDragging())
        {
            const float bodyLeft = destX + (float) cap.bodyBounds.getX();

            g.setColour (colorNeonCyan.withAlpha (0.35f));
            g.fillRect (juce::Rectangle<float> (bodyLeft + 1.0f, sliderPos - 2.0f,
                                                (float) cap.bodyBounds.getWidth() - 2.0f, 5.0f));
        }
    }
    else
    {
        // Asset missing/failed to load — a plain flat rect keeps the fader
        // usable rather than invisible, and makes a build-system wiring
        // mistake obvious immediately instead of silently vanishing.
        g.setColour (CrateColors::LightBackground.brighter (0.15f));
        g.fillRoundedRectangle (juce::Rectangle<float> (centreX - 14.0f, sliderPos - 20.0f, 28.0f, 40.0f), 2.0f);
    }
}

//==============================================================================
// The 3D hardware PAN knob — procedural radial shadow + diagonal metallic cap +
// a carved (dark-then-light) indicator notch.
void CrateMixerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                               float sliderPosProportional, float /*rotaryStartAngle*/, float /*rotaryEndAngle*/,
                                               juce::Slider& slider)
{
    // Filmstrip pan knob — one pre-rendered photorealistic rotation per frame
    // (same hybrid-rendering pivot as the fader cap: zero per-frame procedural
    // drawing, just a sprite-sheet blit).
    const auto& img = getPanKnobFilmstrip();

    if (! img.isValid())
        return;

    // 1. Dynamic bulletproof frame math — hardcoding numFrames (e.g. "49") is
    // exactly the bug this replaces: it silently drifts from whatever the
    // ACTUAL asset contains (this one is 72x9216 = 128 square frames, not 49),
    // which sliced srcH wrong and packed multiple squashed knobs into one
    // "frame". Every filmstrip frame designers export is a perfect square, so
    // the image's WIDTH is exactly one frame's height — the frame count is
    // therefore fully derived from the asset itself, never assumed.
    const int srcW = img.getWidth();
    const int srcH = srcW; // each frame is a perfect square
    const int numFrames = img.getHeight() / srcH;

    if (numFrames <= 0)
        return;

    // 2. Socket size is DECOUPLED from image size — WebKnobMan-style sprite
    // sheets bake significant transparent padding around the visible knob
    // (for the drop-shadow), so sizing the socket off the full image bounds
    // made it comically oversized relative to the actual hardware circle
    // inside it. The image is drawn LARGE (95% of the available space, to
    // compensate for that padding); the socket is tightened to 72% of that —
    // perfectly hugging the matte black knob body (was 65%, slightly loose).
    const float centerX = (float) x + (float) width  * 0.5f;
    const float centerY = (float) y + (float) height * 0.5f;

    const float maxSpace   = juce::jmin ((float) width, (float) height);
    const float imageSize  = maxSpace * 0.95f;
    const float socketSize = imageSize * 0.72f;

    const juce::Rectangle<float> socketRect (centerX - socketSize * 0.5f, centerY - socketSize * 0.5f,
                                             socketSize, socketSize);

    // Premium UX: dynamic neon glow, active ONLY while the user is actually
    // touching the knob — the strictly-rationed accent colour (Manifesto
    // section 1: "2-5% of pixels, used only as a spotlight"), not a permanent
    // fixture. Drawn BEFORE the socket so the socket's opaque fill sits
    // cleanly on top of the glow's outer bloom.
    if (slider.isMouseButtonDown() || slider.isMouseOverOrDragging())
    {
        const auto neonColour = colorNeonCyan.withAlpha (0.18f);
        juce::ColourGradient glow (neonColour, centerX, centerY,
                                    juce::Colours::transparentBlack, centerX, centerY + socketSize * 0.5f + 12.0f, true);
        g.setGradientFill (glow);
        g.fillEllipse (socketRect.expanded (14.0f));
    }

    // Procedural socket, drawn on top of any glow, underneath the image — a
    // pitch-black hardware ring hugging the visible knob so it anchors/pops
    // off the panel instead of floating flush against it.
    g.setColour (CrateColors::DarkBackground.darker (0.4f));
    g.fillEllipse (socketRect);
    g.setColour (CrateColors::LightBackground);
    g.drawEllipse (socketRect, 1.2f);

    // 3. Exact frame index for the current value.
    int frameIndex = (int) std::round (sliderPosProportional * (float) (numFrames - 1));
    frameIndex = juce::jlimit (0, numFrames - 1, frameIndex);

    // 4. Draw the image, snapped to whole pixels to prevent jitter (same
    // reasoning as the fader cap — sub-pixel destination position forces
    // bilinear interpolation/blur on an otherwise-static frame).
    const int destX = juce::roundToInt (centerX - imageSize * 0.5f);
    const int destY = juce::roundToInt (centerY - imageSize * 0.5f);
    const int destSize = juce::roundToInt (imageSize);

    // QA Hardening fix: draw from the PRE-SCALED filmstrip (see
    // getScaledPanKnobFilmstrip's doc comment) — source and destination
    // frame size are now identical, so this is a 1:1 blit, not a per-paint
    // bilinear rescale.
    const auto& scaledImg = getScaledPanKnobFilmstrip (destSize);
    if (scaledImg.isValid())
        g.drawImage (scaledImg,
                     destX, destY, destSize, destSize,
                     0, frameIndex * destSize, destSize, destSize);
}

//==============================================================================
void CrateMixerLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                           int, int, int, int, juce::ComboBox& box)
{
    const juce::Rectangle<float> bounds (0.0f, 0.0f, (float) width, (float) height);

    g.setColour (panelLight);
    g.fillRoundedRectangle (bounds, (float) height * 0.3f);
    g.setColour (panel);
    g.drawRoundedRectangle (bounds.reduced (0.5f), (float) height * 0.3f, 1.0f);

    // Small flat downward triangle — not the OS/JUCE default combo glyph.
    const juce::Rectangle<float> arrowZone ((float) width - 18.0f, 0.0f, 14.0f, (float) height);
    juce::Path arrow;
    arrow.addTriangle (arrowZone.getX(), arrowZone.getCentreY() - 3.0f,
                        arrowZone.getRight(), arrowZone.getCentreY() - 3.0f,
                        arrowZone.getCentreX(), arrowZone.getCentreY() + 3.0f);

    g.setColour (box.isEnabled() ? textDim : textDim.withAlpha (0.4f));
    g.fillPath (arrow);
}
