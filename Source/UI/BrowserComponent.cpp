#include "BrowserComponent.h"
#include "TheCrateLookAndFeel.h"
#include "CrateMidiInspectorComponent.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    constexpr int searchBarHeight = 34;
    constexpr int tabRowHeight    = 30;
    constexpr int rowHeight       = 26;

    const juce::String pluginDragPrefix = "plugin_drag|";

    // Professional iconography (Task 1): real SVG path data, parsed once via
    // juce::Drawable::parseSVGPath and cached as a static juce::Path — NOT
    // hand-rolled fillRect/fillRoundedRectangle calls. Every path is a 24x24
    // viewBox filled silhouette, scaled to fit each button's own bounds at
    // paint time via Path::getTransformToScaleToFit(), so they stay crisp at
    // any tab size.
    const juce::Path& getPlugIconPath()
    {
        static const juce::Path path = juce::Drawable::parseSVGPath (
            "M10 2h1v6h-1zM13 2h1v6h-1z"
            "M7 7h10a1 1 0 0 1 1 1v5a6 6 0 0 1-12 0V8a1 1 0 0 1 1-1z"
            "M11 20h2v3h-2z");
        return path;
    }

    const juce::Path& getWaveformIconPath()
    {
        static const juce::Path path = juce::Drawable::parseSVGPath (
            "M3 10h2v4H3zM7 6h2v12H7zM11 3h2v18h-2zM15 6h2v12h-2zM19 10h2v4h-2z");
        return path;
    }

    const juce::Path& getStarIconPath()
    {
        static const juce::Path path = juce::Drawable::parseSVGPath (
            "M12 2l2.9 6.6 7.1.6-5.4 4.7 1.6 7-6.2-3.9-6.2 3.9 1.6-7L2 9.2l7.1-.6z");
        return path;
    }

    const juce::Path& getSlidersIconPath()
    {
        // Two vertical tracks with a "fader cap" bar at a different height on
        // each — represents the Inspector's dual channel-strip paradigm.
        static const juce::Path path = juce::Drawable::parseSVGPath (
            "M8 3h1v18H8zM16 3h1v18h-1zM5 9h7v2H5zM12 15h7v2h-7z");
        return path;
    }
}

//==============================================================================
void BrowserComponent::IconTabButton::paintButton (juce::Graphics& g, bool isMouseOver, bool /*isButtonDown*/)
{
    // Same on/off colour language the old text tabs used (accent when this is
    // the active tab, dimmed otherwise) — just applied to a Path glyph instead
    // of text. A light hover tint on an inactive tab gives the same "this is
    // clickable" feedback the old TextButton's default hover state gave for free.
    const auto colour = getToggleState() ? LAF::accent
                       : isMouseOver      ? LAF::text
                                          : LAF::textDim;

    const auto iconBounds = getLocalBounds().toFloat().reduced (getWidth() * 0.28f, getHeight() * 0.3f);

    // Each icon's Path is authored in a 24x24 SVG viewBox — getTransformToScaleToFit
    // maps that fixed coordinate space onto this button's actual icon bounds,
    // preserving aspect ratio, so the SAME path renders crisply at any tab size.
    const juce::Path* svgPath = nullptr;
    switch (icon)
    {
        case Icon::Plug:         svgPath = &getPlugIconPath(); break;
        case Icon::Waveform:     svgPath = &getWaveformIconPath(); break;
        case Icon::Star:         svgPath = &getStarIconPath(); break;
        case Icon::MixerSliders: svgPath = &getSlidersIconPath(); break;
    }

    juce::Path scaled (*svgPath);
    scaled.applyTransform (svgPath->getTransformToScaleToFit (iconBounds, true));

    g.setColour (colour);
    g.fillPath (scaled);
}

//==============================================================================
BrowserComponent::SearchBar::SearchBar()
{
    addAndMakeVisible (editor);
    editor.setMultiLine (false);
    editor.setReturnKeyStartsNewLine (false);
    editor.setTextToShowWhenEmpty ("Search plugins, samples...", LAF::textDim);
    editor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    editor.setColour (juce::TextEditor::textColourId, LAF::text);
    editor.setJustification (juce::Justification::centredLeft);
    editor.setBorder (juce::BorderSize<int> (0));
}

void BrowserComponent::SearchBar::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (LAF::panelLight);
    g.fillRoundedRectangle (bounds, bounds.getHeight() * 0.5f);

    // Magnifying-glass glyph, drawn behind the (transparent-background) editor.
    const auto glassBounds = juce::Rectangle<float> (bounds.getX() + 10.0f, bounds.getCentreY() - 5.0f, 8.0f, 8.0f);
    g.setColour (LAF::textDim);
    g.drawEllipse (glassBounds, 1.4f);
    g.drawLine (glassBounds.getRight() - 1.0f, glassBounds.getBottom() - 1.0f,
                glassBounds.getRight() + 3.0f, glassBounds.getBottom() + 3.0f, 1.4f);
}

void BrowserComponent::SearchBar::resized()
{
    editor.setBounds (getLocalBounds().withTrimmedLeft (26).withTrimmedRight (10));
}

//==============================================================================
BrowserComponent::PluginRow::PluginRow (juce::PluginDescription descriptionToUse)
    : description (std::move (descriptionToUse))
{
}

void BrowserComponent::PluginRow::paint (juce::Graphics& g)
{
    if (hovering)
    {
        g.setColour (LAF::accent.withAlpha (0.14f));
        g.fillRect (getLocalBounds());
    }

    const auto height = (float) getHeight();
    const auto iconBounds = juce::Rectangle<float> (8.0f, height * 0.2f, height * 0.6f, height * 0.6f);

    juce::Path scaledIcon (getPlugIconPath());
    scaledIcon.applyTransform (getPlugIconPath().getTransformToScaleToFit (iconBounds, true));
    g.setColour (hovering ? LAF::accent : LAF::textDim);
    g.fillPath (scaledIcon);

    g.setColour (LAF::text);
    g.setFont (juce::FontOptions (12.5f));
    g.drawText (description.name,
                juce::Rectangle<float> (iconBounds.getRight() + 8.0f, 0.0f,
                                         (float) getWidth() - iconBounds.getRight() - 16.0f, height),
                juce::Justification::centredLeft, true);
}

void BrowserComponent::PluginRow::mouseEnter (const juce::MouseEvent&)   { hovering = true; repaint(); }
void BrowserComponent::PluginRow::mouseExit (const juce::MouseEvent&)    { hovering = false; repaint(); }

void BrowserComponent::PluginRow::mouseDown (const juce::MouseEvent&)
{
    dragStarted = false;
}

void BrowserComponent::PluginRow::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted)
        return;

    // JUCE's own standard minimum-drag-distance convention — without this, every
    // plain click (which always involves a few pixels of sub-threshold jitter)
    // would start a drag instead of allowing mouseDoubleClick() to ever fire.
    if (e.getDistanceFromDragStart() < 8)
        return;

    auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this);

    if (container == nullptr || container->isDragAndDropActive())
        return;

    dragStarted = true;

    const juce::var sourceDescription (pluginDragPrefix + description.createIdentifierString());

    // Semi-transparent snapshot of this row as the drag image, per spec.
    auto snapshot = createComponentSnapshot (getLocalBounds());
    snapshot.multiplyAllAlphas (0.6f);

    container->startDragging (sourceDescription, this, juce::ScaledImage (snapshot), true);
}

void BrowserComponent::PluginRow::mouseDoubleClick (const juce::MouseEvent&)
{
    if (onDoubleClicked)
        onDoubleClicked();
}

//==============================================================================
void BrowserComponent::PluginListContent::rebuild (const juce::Array<juce::PluginDescription>& descriptions,
                                                    std::function<void (const juce::PluginDescription&)> onRowDoubleClicked)
{
    rows.clear();

    for (auto& d : descriptions)
    {
        auto row = std::make_unique<PluginRow> (d);
        row->onDoubleClicked = [d, onRowDoubleClicked] { if (onRowDoubleClicked) onRowDoubleClicked (d); };
        addAndMakeVisible (*row);
        rows.push_back (std::move (row));
    }

    relayout();
}

void BrowserComponent::PluginListContent::paint (juce::Graphics& g)
{
    g.fillAll (LAF::background);
}

void BrowserComponent::PluginListContent::relayout()
{
    setSize (getWidth(), juce::jmax ((int) rows.size() * rowHeight, minHeight));
    resized();
}

void BrowserComponent::PluginListContent::resized()
{
    int y = 0;

    for (auto& row : rows)
    {
        row->setBounds (0, y, getWidth(), rowHeight);
        y += rowHeight;
    }
}

//==============================================================================
BrowserComponent::BrowserComponent (CrateWorkflowManager& workflowToUse)
    : workflow (workflowToUse)
{
    addAndMakeVisible (searchBar);
    searchBar.editor.onTextChange = [this] { refreshFilteredPlugins(); };

    // Icon-based tabs (Task 2) — IconTabButton's own paintButton() draws the
    // plug/waveform/star/mixer-sliders glyph directly (accent when toggled on,
    // dimmed otherwise), so there's no LookAndFeel colour styling to apply here
    // the way the old text tabs needed.
    addAndMakeVisible (pluginsTab);
    addAndMakeVisible (samplesTab);
    addAndMakeVisible (favoritesTab);
    addAndMakeVisible (inspectorTab);

    pluginsTab.setTooltip ("Plugins");
    samplesTab.setTooltip ("Samples");
    favoritesTab.setTooltip ("Favorites");
    inspectorTab.setTooltip ("Inspector");

    pluginsTab.onClick    = [this] { setActiveTab (0); };
    samplesTab.onClick    = [this] { setActiveTab (1); };
    favoritesTab.onClick  = [this] { setActiveTab (2); };
    inspectorTab.onClick  = [this] { setActiveTab (3); };

    pluginListContent = std::make_unique<PluginListContent>();
    pluginViewport.setViewedComponent (pluginListContent.get(), false);
    pluginViewport.setScrollBarsShown (true, false); // vertical only
    addAndMakeVisible (pluginViewport);

    addAndMakeVisible (emptyStateLabel);
    emptyStateLabel.setJustificationType (juce::Justification::centredTop);
    emptyStateLabel.setColour (juce::Label::textColourId, LAF::textDim);
    emptyStateLabel.setFont (juce::FontOptions (12.5f));

    // INSPECTOR tab: CrateTrackInspectorComponent is self-contained (no Edit
    // lifecycle wiring), so this class owns it directly — starts hidden,
    // updateContentVisibility() reveals it once tab 3 is actually selected.
    trackInspector = std::make_unique<CrateTrackInspectorComponent> (workflow);
    addChildComponent (*trackInspector);

    // Picks up plugins the engine finds after this component is already showing —
    // the scanner runs on a background thread per MASTER_ARCHITECTURE.md ("The
    // scanner has no UI... never blocks the GUI thread"), so the list it feeds isn't
    // necessarily complete yet at construction time.
    workflow.getEngine().getPluginManager().knownPluginList.addChangeListener (this);

    refreshFilteredPlugins();
    setActiveTab (0);
}

BrowserComponent::~BrowserComponent()
{
    workflow.getEngine().getPluginManager().knownPluginList.removeChangeListener (this);
}

void BrowserComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFilteredPlugins();
}

void BrowserComponent::setActiveTab (int index)
{
    activeTab = index;

    pluginsTab.setToggleState   (index == 0, juce::dontSendNotification);
    samplesTab.setToggleState   (index == 1, juce::dontSendNotification);
    favoritesTab.setToggleState (index == 2, juce::dontSendNotification);
    inspectorTab.setToggleState (index == 3, juce::dontSendNotification);

    if (index == 1 || index == 2)
        emptyStateLabel.setText (index == 1
                                      ? "No samples indexed yet.\nSample library scanning is coming in a later phase."
                                      : "No favorites yet.\nRight-click a plugin or sample to favorite it.",
                                  juce::dontSendNotification);

    updateContentVisibility();
    repaint();
}

void BrowserComponent::setMidiInspector (CrateMidiInspectorComponent* insp)
{
    if (midiInspectorPtr == insp)
        return;

    // Hide/detach the OLD pointer's visibility state before swapping — it's
    // owned by MainComponent, not this class, so this never deletes it, just
    // stops treating it as this tab's content.
    if (midiInspectorPtr != nullptr)
        midiInspectorPtr->setVisible (false);

    midiInspectorPtr = insp;

    if (midiInspectorPtr != nullptr)
        addChildComponent (*midiInspectorPtr); // no-op if already a child (e.g. after a rebuild with the same pointer)

    updateContentVisibility();
    resized();
}

void BrowserComponent::setPianoRollActive (bool isActive)
{
    if (pianoRollActive == isActive)
        return;

    pianoRollActive = isActive;
    updateContentVisibility();

    // Left Panel Isolation: entering/leaving Piano Roll mode changes whether
    // midiInspector gets the full component bounds (isolated) or the normal
    // contentArea beneath the tab header/search bar — needs a real relayout,
    // not just a visibility flip.
    resized();
}

void BrowserComponent::setSelectedTrack (te::Track* t)
{
    if (trackInspector != nullptr)
        trackInspector->setTrack (t);
}

void BrowserComponent::updateContentVisibility()
{
    // Left Panel Isolation (Piano Roll Mode): while the Piano Roll overlay is
    // open, this panel stops being a tabbed browser altogether — the tab
    // header AND search bar hide completely, and midiInspector becomes the
    // ONLY visible content, at the full panel bounds (see resized()). There's
    // nothing else in this panel worth browsing to while the Piano Roll owns
    // the screen, so isolation is total, not just "whichever tab happens to
    // be selected."
    searchBar.setVisible (! pianoRollActive);
    pluginsTab.setVisible (! pianoRollActive);
    samplesTab.setVisible (! pianoRollActive);
    favoritesTab.setVisible (! pianoRollActive);
    inspectorTab.setVisible (! pianoRollActive);

    if (pianoRollActive)
    {
        pluginViewport.setVisible (false);
        emptyStateLabel.setVisible (false);
        trackInspector->setVisible (false);

        if (midiInspectorPtr != nullptr)
            midiInspectorPtr->setVisible (true);

        return;
    }

    const bool showingPlugins    = (activeTab == 0);
    const bool showingEmptyState = (activeTab == 1 || activeTab == 2);
    const bool showingInspector  = (activeTab == 3);

    pluginViewport.setVisible (showingPlugins);
    emptyStateLabel.setVisible (showingEmptyState);
    trackInspector->setVisible (showingInspector);

    if (midiInspectorPtr != nullptr)
        midiInspectorPtr->setVisible (false); // only ever shown during Piano Roll isolation, above
}

void BrowserComponent::refreshFilteredPlugins()
{
    filteredPlugins.clear();

    const auto searchText = searchBar.editor.getText().trim();
    const auto& knownTypes = workflow.getEngine().getPluginManager().knownPluginList.getTypes();

    for (const auto& desc : knownTypes)
        if (searchText.isEmpty() || desc.name.containsIgnoreCase (searchText))
            filteredPlugins.add (desc);

    pluginListContent->rebuild (filteredPlugins, [this] (const juce::PluginDescription& d)
    {
        workflow.loadPluginToSelectedTrack (d);
    });
}

void BrowserComponent::paint (juce::Graphics& g)
{
    // Matches the arrangement's own background exactly — a seamless dock, not a
    // floating panel with its own distinct chrome.
    g.fillAll (LAF::background);

    // No tab header at all during Piano Roll isolation — nothing to underline.
    if (pianoRollActive)
        return;

    // Active-tab underline indicator.
    auto* activeButton = activeTab == 0 ? &pluginsTab
                        : activeTab == 1 ? &samplesTab
                        : activeTab == 2 ? &favoritesTab
                        : &inspectorTab;
    g.setColour (LAF::accent);
    g.fillRect (activeButton->getBounds().withTop (activeButton->getBottom() - 2).withHeight (2));
}

void BrowserComponent::resized()
{
    // Left Panel Isolation (Piano Roll Mode): midiInspector claims the ENTIRE
    // component bounds — no search bar, no tab row, no margins — since those
    // are all hidden (see updateContentVisibility()). Skips the normal
    // tab-header layout below entirely.
    if (pianoRollActive)
    {
        if (midiInspectorPtr != nullptr)
            midiInspectorPtr->setBounds (getLocalBounds());

        return;
    }

    auto area = getLocalBounds().reduced (10, 10);

    searchBar.setBounds (area.removeFromTop (searchBarHeight));
    area.removeFromTop (10);

    auto tabRow = area.removeFromTop (tabRowHeight);
    const int tabWidth = tabRow.getWidth() / 4;
    pluginsTab.setBounds (tabRow.removeFromLeft (tabWidth));
    samplesTab.setBounds (tabRow.removeFromLeft (tabWidth));
    favoritesTab.setBounds (tabRow.removeFromLeft (tabWidth));
    inspectorTab.setBounds (tabRow);

    area.removeFromTop (6);

    // Shared by every possible content view (plugin list, empty state, and the
    // track Inspector) — computed ONCE here so none of them can drift out of
    // sync with each other, same discipline MixerStrip's meterBounds uses.
    contentArea = area;

    pluginViewport.setBounds (contentArea);
    pluginListContent->setMinHeight (contentArea.getHeight());
    pluginListContent->setSize (contentArea.getWidth(), pluginListContent->getHeight()); // apply the new WIDTH first...
    pluginListContent->relayout();                                                       // ...then recompute the correct height from it

    emptyStateLabel.setBounds (contentArea.reduced (4));

    trackInspector->setBounds (contentArea);
}
