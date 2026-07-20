#include "CrateFoldArrow.h"

#include "CrateColors.h"
#include "CrateDesignSystem.h"

void CrateFoldArrow::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool)
{
    namespace DS = CrateDesignSystem::Metrics::TrackHeader;

    const bool expanded = (isExpanded != nullptr) && isExpanded();
    auto b = getLocalBounds().toFloat().reduced (DS::foldArrowInset);

    juce::Path tri;
    if (expanded)
    {
        // Down-pointing (disclosure open).
        tri.addTriangle (b.getX(), b.getY(), b.getRight(), b.getY(), b.getCentreX(), b.getBottom());
    }
    else
    {
        // Right-pointing (disclosure closed).
        tri.addTriangle (b.getX(), b.getY(), b.getRight(), b.getCentreY(), b.getX(), b.getBottom());
    }

    // Desaturated Accents directive: NeonBlue is reserved for active values/
    // automation/the fader thumb — a hover highlight on the fold disclosure
    // glyph isn't any of those, so it just brightens the same neutral grey.
    g.setColour (shouldDrawButtonAsHighlighted ? CrateColors::BrandGray.brighter (0.4f) : CrateColors::BrandGray);
    g.fillPath (tri);
}
