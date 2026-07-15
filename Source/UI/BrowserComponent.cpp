#include "BrowserComponent.h"
#include "TheCrateLookAndFeel.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    constexpr int searchBarHeight = 34;
    constexpr int tabRowHeight    = 30;
    constexpr int rowHeight       = 26;

    const juce::String pluginDragPrefix = "plugin_drag|";

    // Small stylised "plug" glyph (body + two prongs) — stands in for a real
    // per-category icon set until The Crate Brain's ontology DB (section 1) lands;
    // custom-drawn either way, never an OS default icon.
    void drawPlugIcon (juce::Graphics& g, juce::Rectangle<float> bounds, juce::Colour colour)
    {
        g.setColour (colour);
        auto body = bounds.withTop (bounds.getY() + bounds.getHeight() * 0.35f);
        g.fillRoundedRectangle (body, 2.0f);

        const auto prongWidth = bounds.getWidth() * 0.18f;
        g.fillRect (juce::Rectangle<float> (bounds.getX() + bounds.getWidth() * 0.2f, bounds.getY(), prongWidth, bounds.getHeight() * 0.4f));
        g.fillRect (juce::Rectangle<float> (bounds.getRight() - bounds.getWidth() * 0.2f - prongWidth, bounds.getY(), prongWidth, bounds.getHeight() * 0.4f));
    }
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
    drawPlugIcon (g, iconBounds, hovering ? LAF::accent : LAF::textDim);

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

    auto styleTab = [] (juce::TextButton& b)
    {
        b.setClickingTogglesState (false); // driven manually via setActiveTab(), not per-button toggle state
        b.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colours::transparentBlack);
        b.setColour (juce::TextButton::textColourOnId, LAF::accent);
        b.setColour (juce::TextButton::textColourOffId, LAF::textDim);
    };

    addAndMakeVisible (pluginsTab);
    addAndMakeVisible (samplesTab);
    addAndMakeVisible (favoritesTab);
    styleTab (pluginsTab);
    styleTab (samplesTab);
    styleTab (favoritesTab);

    pluginsTab.onClick   = [this] { setActiveTab (0); };
    samplesTab.onClick   = [this] { setActiveTab (1); };
    favoritesTab.onClick = [this] { setActiveTab (2); };

    pluginListContent = std::make_unique<PluginListContent>();
    pluginViewport.setViewedComponent (pluginListContent.get(), false);
    pluginViewport.setScrollBarsShown (true, false); // vertical only
    addAndMakeVisible (pluginViewport);

    addAndMakeVisible (emptyStateLabel);
    emptyStateLabel.setJustificationType (juce::Justification::centredTop);
    emptyStateLabel.setColour (juce::Label::textColourId, LAF::textDim);
    emptyStateLabel.setFont (juce::FontOptions (12.5f));

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

    pluginsTab.setToggleState (index == 0, juce::dontSendNotification);
    samplesTab.setToggleState (index == 1, juce::dontSendNotification);
    favoritesTab.setToggleState (index == 2, juce::dontSendNotification);

    const bool showingPlugins = (index == 0);
    pluginViewport.setVisible (showingPlugins);
    emptyStateLabel.setVisible (! showingPlugins);

    if (! showingPlugins)
        emptyStateLabel.setText (index == 1
                                      ? "No samples indexed yet.\nSample library scanning is coming in a later phase."
                                      : "No favorites yet.\nRight-click a plugin or sample to favorite it.",
                                  juce::dontSendNotification);

    repaint();
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

    // Active-tab underline indicator.
    auto* activeButton = activeTab == 0 ? &pluginsTab : activeTab == 1 ? &samplesTab : &favoritesTab;
    g.setColour (LAF::accent);
    g.fillRect (activeButton->getBounds().withTop (activeButton->getBottom() - 2).withHeight (2));
}

void BrowserComponent::resized()
{
    auto area = getLocalBounds().reduced (10, 10);

    searchBar.setBounds (area.removeFromTop (searchBarHeight));
    area.removeFromTop (10);

    auto tabRow = area.removeFromTop (tabRowHeight);
    const int tabWidth = tabRow.getWidth() / 3;
    pluginsTab.setBounds (tabRow.removeFromLeft (tabWidth));
    samplesTab.setBounds (tabRow.removeFromLeft (tabWidth));
    favoritesTab.setBounds (tabRow);

    area.removeFromTop (6);

    pluginViewport.setBounds (area);
    pluginListContent->setMinHeight (area.getHeight());
    pluginListContent->setSize (area.getWidth(), pluginListContent->getHeight()); // apply the new WIDTH first...
    pluginListContent->relayout();                                               // ...then recompute the correct height from it

    emptyStateLabel.setBounds (area.reduced (4));
}
