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
    spells out an ARGB value for the app's neutral chrome. Semantic STATUS
    colours (Mute red, Solo yellow, meter red/amber/green, record-arm red) are
    deliberately OUTSIDE this 4-colour hierarchy — they encode real state
    (armed/muted/soloed/clipping), not brand chrome, so collapsing them into
    NeonBlue/BrandGray would destroy information the user relies on at a
    glance, not just restyle it.
*/
namespace CrateColors
{
    // Any ACTIVE state: highlights, LED rings, active Send arcs, Pan
    // readouts, toggled-on buttons (that don't have their own semantic
    // colour — see the Mute/Solo/Record carve-out above).
    inline const juce::Colour NeonBlue        { 0xff02d1e8 };

    // Main track backgrounds, neutral plates, standard surface areas.
    inline const juce::Colour LightBackground { 0xff2a2a30 };

    // Void wells, recessed containers, routing blocks, mixer base background.
    inline const juce::Colour DarkBackground  { 0xff16161a };

    // Inactive text, ghost button text, secondary icons, grid lines, disabled elements.
    inline const juce::Colour BrandGray       { 0xff89898e };
}
