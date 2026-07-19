#pragma once

#include <JuceHeader.h>

/**
    The Crate Studio's strict, immutable Brand Identity palette — Lead
    Architect directive: "Global Color Palette Standardization." Exactly FOUR
    colours, no exceptions, no new hex literals introduced anywhere in the app
    outside of this file — the only legitimate exception is a genuinely
    DYNAMIC colour with no fixed brand value (e.g. a user-assigned
    track->getColour()).

    Every other named colour in the codebase (TheCrateLookAndFeel's
    colorHardware/colorTheVoid/colorNeonCyan/colorGhostedOff/etc.) is now
    defined AS one of these four (see TheCrateLookAndFeel.h) rather than its
    own independent hex literal, so there is exactly ONE place that ever
    spells out an ARGB value for the app's neutral chrome.

    Semantic STATUS colours (Mute red, Solo yellow, Record crimson, Playhead
    red) stay conceptually separate from the 4-colour brand hierarchy above —
    they encode real state (armed/muted/soloed/playing), not brand chrome, so
    collapsing them into NeonBlue/BrandGray would destroy information the
    user relies on at a glance. But per the Lead Architect's "Global Color
    Centralization & Purge" directive, they are now ALSO centralized here
    (rather than each panel — TrackHeaderComponent, MixerStrip, InspectorStrip
    — hardcoding its own slightly-different shade), so Solo/Mute/Record read
    identically no matter which panel draws them. TheCrateLookAndFeel.h's
    colorMuteRed/colorSoloYellow are now aliases of these, same one-way
    pattern as the four brand colours above.
*/
namespace CrateColors
{
    // Any ACTIVE state: highlights, LED rings, active Send arcs, Pan
    // readouts, toggled-on buttons (that don't have their own semantic
    // colour — see the status colours below).
    inline const juce::Colour NeonBlue        { 0xff02d1e8 };

    // Main track backgrounds, neutral plates, standard surface areas.
    inline const juce::Colour LightBackground { 0xff2a2a30 };

    // Void wells, recessed containers, routing blocks, mixer base background.
    inline const juce::Colour DarkBackground  { 0xff16161a };

    // Inactive text, ghost button text, secondary icons, grid lines, disabled elements.
    inline const juce::Colour BrandGray       { 0xff89898e };

    // ---- Semantic status colours (see doc comment above) ----

    // Solo ON, everywhere: TrackHeaderComponent, MixerStrip, InspectorStrip.
    inline const juce::Colour SoloYellow    { 0xffffcc00 };

    // Mute ON, everywhere a dedicated Mute control exists.
    inline const juce::Colour MuteRed       { 0xffff3b30 };

    // Record Arm ON, everywhere a dedicated Record control exists.
    inline const juce::Colour RecordCrimson { 0xffdc143c };

    // The Arrangement's transport playhead line. Same hex as MuteRed by
    // design (both are "attention red"), given its own name since the two
    // concepts (transport position vs. mute state) are unrelated and
    // shouldn't read each other's intent from a shared identifier.
    inline const juce::Colour PlayheadRed   { 0xffff3b30 };
}
