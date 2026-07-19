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
        static constexpr float monitorLabelFontSize       = 9.0f;  // MonitorButton (IN/AUTO/OFF)
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
            // Expanded-state header — EXACT hardcoded 300x90 layout (Lead
            // Architect directive — see TrackHeaderComponent::layoutExpanded()).
            static constexpr int headerWidth  = 300;
            static constexpr int headerHeight = 90;
            static constexpr int column1Right = 90;  // Column 1/2 separator (x)
            static constexpr int column2Right = 180; // Column 2/3 separator (x)

            // Column 1 (Identity)
            static constexpr int foldArrowX = 2, foldArrowY = 38, foldArrowW = 14, foldArrowH = 14;
            static constexpr int nameLabelX = 15, nameLabelY = 0, nameLabelW = 75, nameLabelH = 90;

            // Column 2 (Routing) — 5-row tiling, zero dead space in the full
            // 90px column height (18px x 5 = 90).
            static constexpr int col2X = 94, col2W = 82, col2RowH = 18;
            static constexpr int col2ReturnRow1Y = 27, col2ReturnRow2Y = 45; // return-track centred 2-row block: (90-36)/2 = 27
            static constexpr int inputSpecificY  = 18;
            static constexpr int monitorRowY = 36, monitorRowH = 18, monitorGap = 2;
            static constexpr int outputCategoryY = 54, outputSpecificY = 72;

            // Column 3 (Mini-Mixer)
            static constexpr int mutePlateX = 185, mutePlateY = 15, mutePlateW = 40, mutePlateH = 24;
            static constexpr int volumeSliderX = 185, volumeSliderY = 45, volumeSliderW = 40, volumeSliderH = 24;
            static constexpr int soloButtonX = 230, soloButtonY = 15, soloButtonWH = 24;
            static constexpr int recordButtonX = 258, recordButtonY = 15, recordButtonWH = 24;
            static constexpr int panKnobX = 235, panKnobY = 42, panKnobWH = 34;
            static constexpr int meterX = 290, meterY = 8, meterW = 6, meterH = 74;

            static constexpr float meterFloorDb = -60.0f;
            static constexpr float meterRangeDb = 66.0f;

            // Collapsed micro-state
            static constexpr int collapsedPadX = 6, collapsedPadY = 4;
            static constexpr int colourStripW = 3;
            static constexpr int foldArrowCollapsedW = 14;
            static constexpr int meterStripW = 5;
            static constexpr int nameToStripGap = 5;
            static constexpr int collapsedPlateW = 22, collapsedSRW = 18, collapsedGap = 3;

            // Column separators / accent stripe / bottom border
            static constexpr int separatorW      = 1;
            static constexpr int accentStripeW   = 3;
            static constexpr int rowBottomBorderH = 1;

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
