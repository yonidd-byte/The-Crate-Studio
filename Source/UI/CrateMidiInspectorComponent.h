#pragma once

#include <JuceHeader.h>
#include <tracktion_engine/tracktion_engine.h>
#include "TheCrateLookAndFeel.h"

namespace te = tracktion::engine;

class CrateMidiInspectorComponent : public juce::Component
{
public:
    CrateMidiInspectorComponent();
    ~CrateMidiInspectorComponent() override;

    void setActiveClip (te::MidiClip* clip);

    // Callback fired when scale/root note changes (so keyboard can repaint).
    std::function<void()> onScaleChanged;

    // Scale/quantize state — read by PianoRollGridContent for dimming + snap.
    int getRootNote() const noexcept { return rootNoteCombo.getSelectedItemIndex(); }
    int getScaleType() const noexcept { return scaleTypeCombo.getSelectedItemIndex(); }
    bool isSnapToScaleEnabled() const noexcept { return snapToScaleToggle.getToggleState(); }
    bool isStampModeEnabled() const noexcept { return stampModeToggle.getToggleState(); }
    int getChordType() const noexcept { return chordTypeCombo.getSelectedItemIndex(); }
    int getGridResolution() const noexcept { return gridResolutionCombo.getSelectedItemIndex(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    // TIME QUANTIZE.
    juce::ComboBox gridResolutionCombo;
    juce::Slider strengthSlider;  // 0-100, humanize %
    juce::Label strengthLabel { {}, "Strength" };
    juce::Slider swingSlider;     // 0-100
    juce::Label swingLabel { {}, "Swing" };

    // SCALE QUANTIZE.
    juce::ComboBox rootNoteCombo;
    juce::ComboBox scaleTypeCombo;
    juce::ToggleButton snapToScaleToggle { "Snap to Scale" };

    // COLLAPSE MODE.
    juce::ToggleButton collapseEmptyToggle { "Hide Empty Rows" };

    // NOTE PROPERTIES (selected note).
    juce::Slider velocitySlider;  // 1-127
    juce::Label velocityLabel { {}, "Velocity" };
    juce::Slider lengthSlider;    // beat duration
    juce::Label lengthLabel { {}, "Length" };

    // STAMP TOOL SCAFFOLD.
    juce::ToggleButton stampModeToggle { "Stamp Mode" };
    juce::ComboBox chordTypeCombo;

    te::MidiClip* activeClip = nullptr;

    TheCrateLookAndFeel lookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrateMidiInspectorComponent)
};
