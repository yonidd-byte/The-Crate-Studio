#pragma once

#include <JuceHeader.h>

/**
    Column 1's fold/collapse disclosure arrow — draws a right-pointing
    triangle when collapsed, down-pointing when expanded (Cubase/Ableton
    disclosure glyph). Reads its state from the owning header via a callback
    so there's one source of truth (isCollapsed) rather than a second copy
    here. Originally a private nested class of TrackHeaderComponent; pulled
    out here so ArrangementComponent's MasterHeaderRow can share the exact
    same fold-arrow look/behaviour for Master Track Fold Parity.
*/
class CrateFoldArrow : public juce::Button
{
public:
    CrateFoldArrow() : juce::Button ({}) {}
    std::function<bool()> isExpanded; // supplied by the header

private:
    void paintButton (juce::Graphics&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
};
