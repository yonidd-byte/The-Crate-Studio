#pragma once

#include <JuceHeader.h>

/**
    Centralized Design System (Structural Refactor directive) — the single
    source of truth for UI layout dimensions, margins, corner radii, font
    sizes, and one-off colour literals across the channel-strip family:
    TrackHeaderComponent, MixerStrip, MasterStrip, BottomPanelContainer, and
    CrateSendSlot. Pure data — static constexpr only, zero logic, no
    juce::Component dependencies.

    Every value below was extracted VERBATIM from whichever component's own
    previously-local anonymous-namespace constant or inline literal it
    replaces — this is a zero-visual-diff move, not a retuning pass. See each
    component's .cpp for the (now-thin) local aliases that pull these in.

    Metrics::ChannelStrip deliberately UNIFIES the bottom-up level heights
    that MixerStrip and MasterStrip must always share pixel-for-pixel (the
    Fader Alignment directive spent real effort getting these two files to
    agree — nameH, tripletH, panH, etc. were independently duplicated in both
    files with identical values purely by convention). Centralizing them here
    makes that agreement structural instead of a copy-paste discipline: retune
    one value, both strips move together, and the alignment bug that
    motivated the Fader Alignment directive in the first place cannot
    reoccur by one file drifting out of step with the other.

    Semantic brand/status colours (NeonBlue, BrandGray, MuteRed, etc.) still
    live in CrateColors.h — that file's own "exactly four brand colours plus
    status colours, no exceptions" charter is unrelated to this one and stays
    untouched. Colors:: here holds only the handful of genuinely one-off
    literal shades (e.g. Master's violet nameplate) that don't belong in that
    strict brand palette but were still bare hex literals in a .cpp before
    this refactor.

    CRITICAL DIRECTIVE FOR ALL FUTURE CODE: new layout dimensions, pixel
    values, and colours belong here, not as inline magic numbers inside a
    resized()/paint() body.
*/
namespace CrateDesignSystem
{
    namespace Colors
    {
        // One-off literal shades — not part of CrateColors.h's strict brand
        // palette, but also not meant to vary, so they get a name here rather
        // than staying anonymous hex in the middle of a .cpp. Stored as raw
        // ARGB (juce::uint32) since juce::Colour itself isn't a literal type
        // in every JUCE version — construct with juce::Colour (value) at the
        // call site, same pattern CrateColors.h's own hex literals use.
        static constexpr juce::uint32 mixerStripDefaultAccent = 0xff30506a; // MixerStrip::trackAccentColour fallback (no track colour set yet)
        static constexpr juce::uint32 masterNameplateViolet   = 0xff6a3a6a; // MasterStrip's distinct "MASTER" plate fill
        static constexpr juce::uint32 grMeterAmber            = 0xffffb000; // MixerStrip's gain-reduction meter fill
    }

    namespace Typography
    {
        static constexpr float headerNameFontSize        = 13.0f; // TrackHeaderComponent::nameLabel
        static constexpr float stripNameFontSize          = 11.5f; // MixerStrip::trackNameLabel / MasterStrip::nameLabel
        static constexpr float mutePlateFontSize          = 11.5f; // TrackHeaderComponent::MutePlate number glyph
        static constexpr float toggleGlyphFontSize        = 11.0f; // ToggleBlock default ("R"/"S")
        static constexpr float togglePrePostFontSize      = 8.0f;  // ToggleBlock "PRE"/"POST" (return tracks — doesn't fit the single-letter size)
        static constexpr float monitorLabelFontSize       = 11.0f; // MonitorButton (IN/AUTO/OFF) — bold, fills its box Ableton-style, not small/squished
        static constexpr float volumeBarFontSize          = 10.0f; // TrackHeaderComponent::VolumeBar dB readout
        static constexpr float dbReadoutFontSize          = 9.5f;  // fader/peak dB readout boxes (Mixer + Master)
        static constexpr float panValueFontSize           = 9.0f;  // pan value readout ("C" / "20 L")
        static constexpr float sendChipFontSize           = 11.0f; // CrateSendSlot destination chip
        static constexpr float sendsCaptionFontSize       = 9.0f;  // "SENDS" section caption
        static constexpr float sendsEmptyCaptionFontSize  = 9.5f;  // "(none)" placeholder caption
        static constexpr float outputSlotFontSize         = 11.0f; // Master's read-only "Stereo Out" label
        static constexpr float groupSlotFontSize          = 11.0f; // RoutingBlock's Group chip
        static constexpr float trackIconGlyphFontSize     = 12.0f; // MixerStrip's embedded note glyph (♪)
        static constexpr float midiFxPlaceholderFontSize  = 13.0f; // BottomPanelContainer's "coming soon" label
    }

    namespace Metrics
    {
        // Shared bottom-up level heights/gaps — MixerStrip AND MasterStrip
        // (and, for the meter range, TrackHeaderComponent too) all read the
        // SAME values from here. See this file's own doc comment for why
        // that unification matters.
        namespace ChannelStrip
        {
            static constexpr int levelGap      = 4;
            static constexpr int nameH         = 20;  // L1 name plate
            static constexpr int tripletH      = 22;  // R/S/I triad row (blank on Master)
            static constexpr int dbReadoutH    = 16;  // L5 dB readout boxes
            static constexpr int panH          = 42;  // L6 pan knob
            static constexpr int scribbleIconH = 22;  // embedded track-type icon slot (blank on Master)
            static constexpr int scribbleGap   = 2;
            static constexpr int panValueH     = 12;  // pan value readout label
            static constexpr int panValueGap   = 2;
            static constexpr int routingRowH   = 22;  // L9 Routing — single row baseline
            static constexpr int compH         = 24;  // L12 Channel Comp block
            static constexpr int eqH           = 60;  // L13 EQ display
            static constexpr int settingsH     = 22;  // L14 Settings button
            static constexpr int outerMargin   = 4;
            static constexpr int meterColumnWidth = 22;
            static constexpr float meterFloorDb = -60.0f;
            static constexpr float meterRangeDb = 66.0f; // floor to +6 dB headroom
        }

        // MixerStrip-only additions (Master has no Sends, no dynamic
        // Routing well height, no GR meter).
        namespace Mixer
        {
            static constexpr int faderMinH        = 130; // L4 fader block minimum (it stretches past this)
            static constexpr int routingRowGap     = 2;   // gap between OUT1+2 and the Group chip, when shown
            static constexpr int sendsH            = 62;  // L10 Sends section
            static constexpr int rackMargin        = 6;   // "Universal Rack Width" — positions every dark rack container
            static constexpr int rackButtonPadding = 4;
            static constexpr int sendsToRoutingGap = 8;    // strict gap so the Sends/Routing wells never touch
            static constexpr int grMeterColumnWidth = 8;
            static constexpr float grRangeDb        = 20.0f;
            static constexpr float wellCornerRadius = 3.0f; // Routing/Sends "Void Well" rounded corners
            static constexpr int triadGap           = 2;    // R/S/I triad inner gaps
            static constexpr int sendRowH           = 22;   // one CrateSendSlot row, inside SendsSection::Content
            static constexpr int sendRowGap         = 2;
            static constexpr int sendsCaptionH      = 12;
            static constexpr int sendsCaptionBottomGap = 2;
            static constexpr int sendsScrollbarThickness = 8;
            static constexpr int trackIconCornerRadius = 3;
        }

        // MasterStrip-only additions.
        namespace Master
        {
            static constexpr float muteThresholdDb     = -90.0f; // getVolumeDb() <= this reads as "muted"
            static constexpr int   selectedAccentStripeW = 3;    // left accent stripe when selected
        }

        namespace TrackHeader
        {
            // Content-Driven Dynamic Height directive: the old fixed 300x90
            // grid is GONE — height is now computed from which rows are
            // actually visible (see TrackHeaderComponent::computePreferredHeight()).
            // Width stays fixed (columns are still a strict 3-column grid
            // horizontally); only HEIGHT flexes with content, same as Ableton's
            // own track headers. rowH is the one uniform grid unit every
            // Column 2 row (and Column 3's Solo/Record/Volume row) is built
            // from, so the whole grid stays mathematically consistent no
            // matter how many rows are actually showing.
            static constexpr int headerWidth  = 300;
            static constexpr int column1Right = 90;  // Column 1/2 separator (x)
            static constexpr int column2Right = 180; // Column 2/3 separator (x)
            // 13px Height Lock directive: matches Column 3's Box A/B/C row
            // height EXACTLY, so Column 2 and Column 3 rows form one
            // continuous, perfectly aligned grid line. Also read by
            // MasterHeaderRow's Cue Out / Master Out combos.
            static constexpr int rowH         = 13;  // uniform Column 2/3 row unit

            // ComboBox id conventions this file's height math depends on —
            // shared with TrackHeaderComponent's own combo item ids (see its
            // constructor): id 1 is always "No Input" / "Master" respectively.
            static constexpr int noInputCategoryId     = 1;
            static constexpr int masterOutputCategoryId = 1;

            // Column 2 (Routing) — dynamic top-down stack. Each row is exactly
            // rowH tall, flush against both divider lines (Strict I/O Grid
            // directive — no 4px inset, the combo's own 1px border IS the grid
            // line's edge). Row COUNT varies 2-5 depending on input/output
            // category (see computePreferredHeight()):
            //   input:  category (always) + specific (hidden if No Input)
            //   monitor: IN/AUTO/OFF triad (hidden if No Input)
            //   output: category (always) + specific (hidden if Master)
            static constexpr int col2X = 90, col2W = 90;

            // Column 2 Row Gaps directive: a strict vertical gap BETWEEN
            // consecutive rows (not after the last one) — the earlier "zero
            // padding, flush against the divider lines" pass over-corrected;
            // Ableton's own reference shows a real 1-2px seam between rows.
            // computePreferredHeight() must count these gaps too, or the
            // dynamic-height stack would overflow/overlap Column 3's content.
            static constexpr int col2RowGap = 4; // Exact Math directive — matches Column 3's own inter-row gap exactly

            // Column 2 Breathing Room directive: a strict 2px inset on ALL
            // FOUR sides of Column 2's bounding box (left/right against the
            // Column 1/2 and Column 2/3 divider lines; top/bottom against
            // the header's own edges) — rows no longer stretch flush to any
            // boundary. computePreferredHeight() adds 2x this (top + bottom)
            // to its column2Height total so nothing gets cut off.
            static constexpr int col2Inset = 2;

            // Column 3 (Mini-Mixer), MasterHeaderRow-only block — Master's own
            // resized() still uses these three (its blank Solo/Record
            // placeholder row + real Volume/Pan) since the Ableton Geometry
            // directive below is scoped to TrackHeaderComponent specifically.
            static constexpr int col3RowAH  = 24; // Box A/B/C row height (Master only)
            static constexpr int muteBoxW   = 40; // Master's Volume width (Master only)
            static constexpr int volumeRowH = 24; // Master only
            static constexpr int panSize    = 34; // Master only

            // Ableton Geometry directive — TrackHeaderComponent's Column 3 is
            // now absolute, hardcoded pixel math (see its own resized()), not
            // a derived/relative layout. These two are the ONLY named
            // Column 3 constants that class still reads — everything inside
            // Column 3's own bounds is literal integer arithmetic in the
            // .cpp, by explicit directive ("hardcode these integer
            // relationships", not a further layer of named sub-constants).
            static constexpr int col3Width  = 119;
            static constexpr int col3Height = 86; // driven by the meter: 2 + 82 + 2
            static constexpr int column3FixedHeight = col3Height; // the floor under Column 2's dynamic range

            // Return tracks: collapsed = ONE compact row (name/S/Post-Pre
            // only, no dropdowns at all); expanded = routing dropdown + the
            // SAME Column 3 fixed block as a regular track (never
            // Arm/Monitor — a return can't record or monitor an input).
            static constexpr int returnCollapsedHeight = 26;
            static constexpr int returnExpandedHeight  = column3FixedHeight; // routing row (rowH) fits inside it

            // Regular track collapsed micro-state — unchanged single-strip height.
            static constexpr int collapsedHeight = 30;

            // Column 1 (Identity Only) — strictly the fold arrow + track name
            // on the full-height opaque trackColor fill. The track number /
            // Mute Plate lives in Column 3 (Box A) — see the Column 3 doc
            // comment above.
            static constexpr int foldArrowSize = 14;
            static constexpr int identityPad   = 4; // inset from column1's own edges

            static constexpr float meterFloorDb = -60.0f;
            static constexpr float meterRangeDb = 66.0f;

            // Column separators / accent stripe / bottom border. Strict
            // Bordered Grid directive: these are opaque 2px black cuts —
            // absolute separation between UI zones, not a felt hairline.
            // accentStripeW stays 3 for MasterHeaderRow's own selected-accent
            // stripe (unaffected by this directive, which is scoped to
            // TrackHeaderComponent — see its own paint()).
            static constexpr int separatorW      = 2;
            static constexpr int accentStripeW   = 3;
            static constexpr int rowBottomBorderH = 2;

            // Still used by MasterHeaderRow's own hairline dividers (out of
            // this directive's scope) and nowhere in TrackHeaderComponent any
            // more — its own strokes are all opaque black now (see paint()
            // and the ToggleBlock/MutePlate/VolumeBar/PanBar paintButton()s).
            static constexpr float hairlineAlpha  = 0.3f;

            // ToggleBlock / MutePlate / MonitorButton paint() highlight deltas
            static constexpr float pressedBrighten  = 0.15f; // shouldDrawButtonAsDown, all three
            static constexpr float hoverBrighten     = 0.08f; // ToggleBlock / MonitorButton hover
            static constexpr float muteHoverBrighten = 0.10f; // MutePlate hover (slightly stronger)
            static constexpr float offStateBrighten  = 0.1f;  // LightBackground.brighter() for the "not lit" flat box

            static constexpr float foldArrowInset = 3.0f; // FoldArrow triangle inset from its own bounds

            // VolumeBar
            static constexpr float volumeBarCornerRadius = 2.0f;
            static constexpr float volumeBarFillAlpha = 0.85f;
            static constexpr float volumeBarDragPixelsForFullRange = 200.0f;

            // Desaturated Accents directive: track-number chip and AUTO
            // monitor state use a muted console amber, not NeonBlue/SoloYellow
            // — NeonBlue is reserved for active fader/automation values only.
            static constexpr juce::uint32 mutedAmber = 0xffc98a3d;
        }

        namespace SendSlot
        {
            static constexpr float destinationChipRatio = 0.30f; // knob's share — "70% destination / 30% knob"
            static constexpr int   knobGap = 3;                  // gap between chip and knob
            static constexpr int   verticalReduce = 1;           // getLocalBounds().reduced (0, 1)
            static constexpr float dimAlpha = 0.4f;               // bypassed chip text alpha
            static constexpr float bypassedDarken = 0.3f;         // bypassed chip fill darken factor
        }

        namespace BottomPanel
        {
            static constexpr int preferredHeight = 235; // fixed height regardless of active view
        }
    }
}
