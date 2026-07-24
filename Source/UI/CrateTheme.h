#pragma once
/*
  CrateTheme.h — The Crate Studio design system as native JUCE.
  "Flat with Micro-Depth": Void background, Hardware panels, sunken fader
  grooves, popped caps, Neon Blue strictly rationed to hover/active states.

  Derived from the v2 target reference render + Crate brand tokens.
  NOTE: the-crate-studio-sketch-v2.jsx was not received; hex values below are
  the canonical brand tokens. Diff against the JSX when re-uploaded.

  Rendering contract (see ADDENDUM Part 6):
    - JUCE 8 Direct2D backend on Windows. No juce::OpenGLContext on the UI.
    - All shadow/glow assets pre-rendered via CrateSprites, blitted in paint().
    - Every component here is setOpaque(true) unless stated otherwise.
*/

#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
namespace CrateColors
{
    // Core surfaces
    inline const juce::Colour Void          { 0xff16161a };  // app background
    inline const juce::Colour Hardware      { 0xff2a2a30 };  // panels / strips
    inline const juce::Colour HardwareDeep  { 0xff202026 };  // sunken wells (sends box, groove)
    inline const juce::Colour HardwareEdge  { 0xff3a3a42 };  // 1px panel separation lines

    // Text & ticks
    inline const juce::Colour BrandGray     { 0xff89898e };  // labels, ruler ticks
    inline const juce::Colour BrandGrayDim  { 0xff5a5a60 };  // idle/ghosted labels
    inline const juce::Colour TextBright    { 0xffd6d6da };  // primary values only

    // Accent — hard budget: ~2% of visible pixels. Hover, active LED,
    // active transport text, playhead. NEVER fills, NEVER backgrounds.
    inline const juce::Colour NeonBlue      { 0xff02d1e8 };
    inline const juce::Colour NeonBlueDim   = NeonBlue.withAlpha (0.35f); // automation overlay dim

    // Meters (target render: green body -> amber -> red crest, no gradient per frame)
    inline const juce::Colour MeterGreen    { 0xff3ddc5a };
    inline const juce::Colour MeterAmber    { 0xffe8b902 };
    inline const juce::Colour MeterRed      { 0xffe83a3a };

    // Special
    inline const juce::Colour MasterPurple  { 0xff7a4fd8 };  // master track tag
    inline const juce::Colour RecordRed     { 0xffe8443a };

    // Legacy CrateColors.h aliases — CrateColors.h is retired, no old hex
    // literals reintroduced. Existing call sites keep compiling, remapped
    // onto the nearest semantic token in the new palette above.
    inline const juce::Colour& LightBackground = Hardware;   // panels / strips
    inline const juce::Colour& DarkBackground  = Void;       // absolute background
    inline const juce::Colour& SoloYellow      = MeterAmber; // solo-on state
    inline const juce::Colour& MuteRed         = MeterRed;   // mute-on state
    inline const juce::Colour& RecordCrimson   = RecordRed;  // record-arm state
    inline const juce::Colour& PlayheadRed     = MeterRed;   // transport playhead line
}

//==============================================================================
namespace CrateMetrics
{
    constexpr int   panelRadius      = 6;     // rounded panel corners (px @ 1x)
    constexpr int   controlRadius    = 4;
    constexpr int   grooveWidth      = 6;     // sunken fader track width
    constexpr int   faderCapW        = 22;
    constexpr int   faderCapH        = 12;
    constexpr int   stripWidth       = 128;   // channel strip fixed width
    constexpr int   deviceChainH     = 168;   // bottom device chain height
    constexpr int   transportH       = 44;
    constexpr int   browserW         = 264;
    constexpr float hoverGlowRadius  = 9.0f;
    constexpr int   animMs           = 140;   // panel slide / crossfade duration
}

//==============================================================================
/*  Pre-rendered shadow/glow sprites. Generated ONCE per DPI scale (regenerate
    from the Phase-4 DPI bridge notification). paint() only ever blits these. */
class CrateSprites
{
public:
    static CrateSprites& get() { static CrateSprites s; return s; }

    void rebuildForScale (float scale)
    {
        using namespace CrateColors;
        const auto s = [scale] (float v) { return juce::roundToInt (v * scale); };

        // Neon hover glow (radial falloff, alpha-only, tinted at blit time)
        {
            const int r = s (CrateMetrics::hoverGlowRadius), d = r * 2 + 2;
            hoverGlow = juce::Image (juce::Image::ARGB, d, d, true);
            juce::Graphics g (hoverGlow);
            for (int i = r; i > 0; --i)
            {
                const float t = (float) i / (float) r;                    // 1 -> edge
                g.setColour (NeonBlue.withAlpha (0.16f * (1.0f - t) * (1.0f - t)));
                g.fillEllipse ((float) (r - i), (float) (r - i), (float) (i * 2), (float) (i * 2));
            }
        }

        // Fader cap drop shadow (soft 2px offset)
        {
            const int w = s ((float) CrateMetrics::faderCapW + 8);
            const int h = s ((float) CrateMetrics::faderCapH + 8);
            capShadow = juce::Image (juce::Image::ARGB, w, h, true);
            juce::Graphics g (capShadow);
            juce::DropShadow ds (juce::Colours::black.withAlpha (0.55f), s (3.0f), { 0, s (2.0f) });
            juce::Path cap;
            cap.addRoundedRectangle (s (4.0f), s (3.0f),
                                     s ((float) CrateMetrics::faderCapW),
                                     s ((float) CrateMetrics::faderCapH), s (2.0f));
            ds.drawForPath (g, cap);   // the ONLY place DropShadow ever runs — offline
        }
    }

    juce::Image hoverGlow, capShadow;
private:
    CrateSprites() { rebuildForScale (1.0f); }
};

//==============================================================================
class CrateLookAndFeel : public juce::LookAndFeel_V4
{
public:
    CrateLookAndFeel()
    {
        using namespace CrateColors;
        setColour (juce::ResizableWindow::backgroundColourId, Void);
        setColour (juce::Label::textColourId,                 BrandGray);
        setColour (juce::TextButton::buttonColourId,          juce::Colours::transparentBlack); // ghosted nav
        setColour (juce::TextButton::textColourOffId,         BrandGrayDim);
        setColour (juce::TextButton::textColourOnId,          NeonBlue);
        setColour (juce::PopupMenu::backgroundColourId,       Hardware);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, HardwareDeep);
        setColour (juce::PopupMenu::highlightedTextColourId,  TextBright);
        setColour (juce::TooltipWindow::backgroundColourId,   Hardware);
    }

    juce::Font getLabelFont (juce::Label&) override
    {
        return { juce::FontOptions ("Poppins", 12.0f, juce::Font::plain) };
    }

    //==========================================================================
    // Ghosted top-nav / transport buttons: transparent idle, Neon text+underline
    // when active, faint Hardware fill + glow edge on hover. No filled blocks.
    void drawButtonBackground (juce::Graphics& g, juce::Button& b,
                               const juce::Colour&, bool isOver, bool isDown) override
    {
        using namespace CrateColors;
        auto r = b.getLocalBounds().toFloat().reduced (1.0f);

        if (isDown || isOver)
        {
            g.setColour (Hardware.withAlpha (isDown ? 0.9f : 0.55f));
            g.fillRoundedRectangle (r, (float) CrateMetrics::controlRadius);
        }
        if (b.getToggleState())   // active view tab: 2px Neon underline only
        {
            g.setColour (NeonBlue);
            g.fillRect (r.removeFromBottom (2.0f).reduced (r.getWidth() * 0.28f, 0.0f));
        }
    }

    //==========================================================================
    // Rotary (pan / send knobs): flat Hardware puck, thin BrandGray tick,
    // tick turns Neon on hover/drag. Arc geometry cached by the owner component.
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                           float pos, float startAngle, float endAngle,
                           juce::Slider& s) override
    {
        using namespace CrateColors;
        auto bounds  = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (3.0f);
        auto radius  = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
        auto centre  = bounds.getCentre();
        auto angle   = startAngle + pos * (endAngle - startAngle);
        bool hot     = s.isMouseOverOrDragging();

        if (hot)  // pre-rendered glow blit, tinted — never a live blur
            g.drawImage (CrateSprites::get().hoverGlow,
                         bounds.expanded (CrateMetrics::hoverGlowRadius));

        g.setColour (HardwareDeep);                       // recessed ring
        g.fillEllipse (centre.x - radius, centre.y - radius, radius * 2, radius * 2);
        g.setColour (Hardware);                           // puck
        auto puck = radius - 2.0f;
        g.fillEllipse (centre.x - puck, centre.y - puck, puck * 2, puck * 2);

        juce::Path tick;                                  // pointer
        tick.addRectangle (-1.0f, -puck + 2.0f, 2.0f, puck * 0.55f);
        g.setColour (hot ? NeonBlue : BrandGray);
        g.fillPath (tick, juce::AffineTransform::rotation (angle).translated (centre));
    }

    //==========================================================================
    // Linear fader: 6px sunken groove (hard inset — dark top/left, light
    // bottom/right lines, no gradients), cap blits its cached shadow sprite.
    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                           float sliderPos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        using namespace CrateColors;
        if (style != juce::Slider::LinearVertical)
            { LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, 0, 0, style, s); return; }

        const float cx = (float) x + (float) w * 0.5f;
        juce::Rectangle<float> groove (cx - CrateMetrics::grooveWidth * 0.5f, (float) y,
                                       (float) CrateMetrics::grooveWidth, (float) h);

        g.setColour (HardwareDeep);                                   // well
        g.fillRoundedRectangle (groove, 2.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));          // inset top/left
        g.drawLine (groove.getX(), groove.getY(), groove.getRight(), groove.getY(), 1.5f);
        g.drawLine (groove.getX(), groove.getY(), groove.getX(), groove.getBottom(), 1.5f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));         // lip bottom/right
        g.drawLine (groove.getX(), groove.getBottom(), groove.getRight(), groove.getBottom(), 1.0f);

        // Cap: shadow sprite first, then flat body + centre line
        juce::Rectangle<float> cap (cx - CrateMetrics::faderCapW * 0.5f,
                                    sliderPos - CrateMetrics::faderCapH * 0.5f,
                                    (float) CrateMetrics::faderCapW,
                                    (float) CrateMetrics::faderCapH);
        g.drawImage (CrateSprites::get().capShadow, cap.expanded (4.0f, 4.0f));
        g.setColour (Hardware.brighter (0.25f));
        g.fillRoundedRectangle (cap, 2.0f);
        g.setColour (s.isMouseOverOrDragging() ? NeonBlue : BrandGray);
        g.fillRect (cap.reduced (3.0f, 0.0f).withHeight (1.5f)
                       .withY (cap.getCentreY() - 0.75f));
    }
};

//==============================================================================
/*  Level meter: opaque, three flat colour bands (no gradient objects in paint),
    driven by ONE shared VBlankAttachment owner that polls a lock-free FIFO
    written by the audio thread. repaint() fires only when the drawn height
    changes by >= 0.5px. See ADDENDUM 6.2. */
class CrateMeter : public juce::Component
{
public:
    CrateMeter() { setOpaque (true); }

    // called from the shared VBlank tick with the latest atomic peak (0..1)
    void push (float newLevel01)
    {
        if (std::abs (newLevel01 - level) * (float) getHeight() >= 0.5f)
            { level = newLevel01; repaint(); }
        else level = newLevel01;
    }

    void paint (juce::Graphics& g) override
    {
        using namespace CrateColors;
        auto b = getLocalBounds().toFloat();
        g.fillAll (HardwareDeep);

        const float lit = b.getHeight() * level;
        auto litArea    = b.removeFromBottom (lit);

        // flat stacked bands: green to -12 dBFS, amber to -3, red above
        const float amberFrom = b.getHeight() + lit - (0.80f * (float) getHeight());
        const float redFrom   = b.getHeight() + lit - (0.94f * (float) getHeight());

        g.setColour (MeterGreen);  g.fillRect (litArea);
        if (level > 0.80f) { g.setColour (MeterAmber);
                             g.fillRect (litArea.withTop (juce::jmax (litArea.getY(), amberFrom))); }
        if (level > 0.94f) { g.setColour (MeterRed);
                             g.fillRect (litArea.withTop (juce::jmax (litArea.getY(), redFrom))); }
    }

private:
    float level = 0.0f;
};

//==============================================================================
/*  Channel strip layout — the resized() math matching the v2 reference:
    fixed-width column; header, curve display, sends well and IO rows stack
    with FlexBox; fader row splits fader | meter side by side. */
class CrateChannelStripLayout
{
public:
    struct Slots { juce::Rectangle<int> header, curve, comp, sends,
                                        input, output, pan, readout, fader, meter, muteSolo; };

    static Slots layout (juce::Rectangle<int> bounds)
    {
        Slots s;
        auto area   = bounds.reduced (8);
        s.header    = area.removeFromTop (26);
        area.removeFromTop (6);
        s.curve     = area.removeFromTop (52);          // channel-comp curve card
        s.comp      = area.removeFromTop (18);
        area.removeFromTop (8);
        s.sends     = area.removeFromTop (56);          // sunken sends well
        area.removeFromTop (8);
        s.input     = area.removeFromTop (20);
        s.output    = area.removeFromTop (20);
        area.removeFromTop (10);
        s.pan       = area.removeFromTop (44);
        s.readout   = area.removeFromTop (16);          // 0.0 / -inf row
        area.removeFromTop (6);
        s.muteSolo  = area.removeFromBottom (24);
        area.removeFromBottom (6);

        // fader | meter split via FlexBox (fader flexes, meter fixed 10px)
        juce::FlexBox fb;
        fb.flexDirection = juce::FlexBox::Direction::row;
        juce::Component faderProxy, meterProxy;         // proxies for rect math
        fb.items = { juce::FlexItem (faderProxy).withFlex (1.0f),
                     juce::FlexItem (meterProxy).withWidth (10.0f)
                                                .withMargin ({ 0, 0, 0, 6 }) };
        fb.performLayout (area);
        s.fader = faderProxy.getBounds();
        s.meter = meterProxy.getBounds();
        return s;
    }
};
