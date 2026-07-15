#include "TransportBar.h"
#include "SettingsComponent.h"
#include "PluginBrowserComponent.h"
#include "TheCrateLookAndFeel.h"

namespace
{
    using LAF = TheCrateLookAndFeel;

    const auto backgroundColour = juce::Colour (0xff141416);
    const auto idleIconColour   = LAF::text;
    const auto playActiveColour = juce::Colour (0xff2e8b3d);
    const auto loopActiveColour = juce::Colour (0xff3d7fff);
    const auto recordIdleColour = LAF::textDim;
    const auto recordActiveColour = juce::Colour (0xffff3b30);

    constexpr int midiActivityHoldMs = 150;

    // Every icon is drawn inside a square, slightly inset, centred bounds — keeps
    // all six glyphs visually the same weight regardless of shape.
    juce::Rectangle<float> iconArea (juce::Rectangle<int> bounds)
    {
        return bounds.toFloat().reduced (juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.3f);
    }

    // Shared by Loop/Undo/Redo: a stroked arc with an arrowhead tangent to the
    // curve at its end (pointing along the direction of travel), not radially
    // outward — a radial arrowhead is what made the previous pass's loop/undo/
    // redo glyphs read as a deformed blob instead of a clean directional arrow.
    void drawArcArrow (juce::Graphics& g, juce::Point<float> centre, float radius,
                        float startAngle, float endAngle, juce::Colour colour)
    {
        juce::Path arc;
        arc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, startAngle, endAngle, true);
        g.setColour (colour);
        g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const juce::Point<float> tip (centre.x + std::sin (endAngle) * radius,
                                       centre.y - std::cos (endAngle) * radius);

        // Direction of travel along the arc at its end point, plus the
        // perpendicular (normal) direction, used to build a small triangle whose
        // tip sits exactly on the arc and whose base is tangent to it.
        const float tangent = endAngle + juce::MathConstants<float>::halfPi;
        const float headLen = radius * 0.5f;
        const juce::Point<float> back (tip.x - std::sin (tangent) * headLen, tip.y + std::cos (tangent) * headLen);
        const juce::Point<float> normal (std::cos (tangent) * headLen * 0.55f, std::sin (tangent) * headLen * 0.55f);

        juce::Path head;
        head.addTriangle (tip.x, tip.y, back.x + normal.x, back.y + normal.y, back.x - normal.x, back.y - normal.y);
        g.fillPath (head);
    }
}

//==============================================================================
void TransportBar::IconButton::paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = getLocalBounds().toFloat();

    auto bg = (icon == Icon::loop && getToggleState()) ? loopActiveColour : LAF::panelLight;

    if (shouldDrawButtonAsDown)
        bg = bg.brighter (0.2f);
    else if (shouldDrawButtonAsHighlighted)
        bg = bg.brighter (0.1f);

    if (! isEnabled())
        bg = bg.withMultipliedAlpha (0.4f);

    g.setColour (bg);
    g.fillRect (bounds); // flat square block, no rounding — matches the R/S/M/A toggle language

    const auto area = iconArea (getLocalBounds());
    g.setColour (isEnabled() ? iconColour : iconColour.withAlpha (0.4f));

    switch (icon)
    {
        case Icon::play:
        {
            // Strict equilateral triangle, apex pointing right, centred in `area`.
            const auto side = juce::jmin (area.getWidth(), area.getHeight());
            const auto h = side * 0.5f;
            juce::Path p;
            p.addTriangle (area.getCentreX() - h * 0.55f, area.getCentreY() - h,
                            area.getCentreX() - h * 0.55f, area.getCentreY() + h,
                            area.getCentreX() + h * 0.85f, area.getCentreY());
            g.fillPath (p);
            break;
        }

        case Icon::stop:
        {
            // Strict square, no rounding.
            const auto side = juce::jmin (area.getWidth(), area.getHeight());
            g.fillRect (juce::Rectangle<float> (side, side).withCentre (area.getCentre()));
            break;
        }

        case Icon::record:
        {
            // Perfect circle.
            const auto diameter = juce::jmin (area.getWidth(), area.getHeight());
            g.fillEllipse (juce::Rectangle<float> (diameter, diameter).withCentre (area.getCentre()));
            break;
        }

        case Icon::loop:
        {
            const auto centre = area.getCentre();
            const auto radius = area.getWidth() * 0.5f;
            drawArcArrow (g, centre, radius,
                          juce::MathConstants<float>::pi * 0.15f,
                          juce::MathConstants<float>::pi * 1.85f,
                          iconColour);
            break;
        }

        case Icon::undo:
        case Icon::redo:
        {
            const bool flip = (icon == Icon::redo);
            const auto centre = area.getCentre();
            const auto radius = area.getWidth() * 0.42f;
            const auto start = flip ? juce::MathConstants<float>::pi * 1.25f : -juce::MathConstants<float>::pi * 0.25f;
            const auto end   = flip ? juce::MathConstants<float>::pi * 2.25f : juce::MathConstants<float>::pi * 1.25f;
            drawArcArrow (g, centre, radius, start, end, iconColour);
            break;
        }
    }
}

//==============================================================================
void TransportBar::LcdDisplay::setValues (const juce::String& barsBeats, const juce::String& time)
{
    if (barsBeatsText == barsBeats && timeText == time)
        return;

    barsBeatsText = barsBeats;
    timeText = time;
    repaint();
}

void TransportBar::LcdDisplay::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (LAF::lcdBackground);
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (LAF::panelLight);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);

    auto area = getLocalBounds().reduced (8, 2);
    auto barsArea = area.removeFromLeft (area.getWidth() * 6 / 10);
    area.removeFromLeft (3);
    g.setColour (LAF::panelLight);
    g.fillRect (juce::Rectangle<int> (barsArea.getRight() + 1, area.getY(), 1, area.getHeight()));

    // Small, monospaced-style digits — this is a compact status readout, not a
    // hero display; it must fit the 24px-max bounding box resized() gives it.
    g.setColour (LAF::lcdText);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::bold));
    g.drawText (barsBeatsText, barsArea, juce::Justification::centredLeft);

    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain));
    g.drawText (timeText, area, juce::Justification::centredLeft);
}

//==============================================================================
void TransportBar::CpuMeter::setLoad (float proportion0to1)
{
    const auto clamped = juce::jlimit (0.0f, 1.0f, proportion0to1);

    if (std::abs (clamped - load) < 0.005f)
        return;

    load = clamped;
    repaint();
}

void TransportBar::CpuMeter::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (LAF::lcdBackground);
    g.fillRoundedRectangle (bounds, 2.0f);

    auto fill = bounds.reduced (1.5f);
    fill.setWidth (fill.getWidth() * load);
    g.setColour (load > 0.85f ? LAF::meterHot : LAF::accent);
    g.fillRoundedRectangle (fill, 1.5f);

    g.setColour (LAF::panelLight);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 2.0f, 1.0f);
}

//==============================================================================
void TransportBar::DraggableBpmLabel::setDisplayText (const juce::String& text)
{
    if (displayText == text)
        return;

    displayText = text;
    repaint();
}

double TransportBar::DraggableBpmLabel::currentBpm() const
{
    const auto& tempos = workflow.getEdit().tempoSequence.getTempos();
    return tempos.isEmpty() ? 120.0 : tempos.getFirst()->getBpm();
}

void TransportBar::DraggableBpmLabel::paint (juce::Graphics& g)
{
    g.setColour (LAF::text);
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    g.drawText (displayText, getLocalBounds(), juce::Justification::centredRight);
}

void TransportBar::DraggableBpmLabel::mouseDown (const juce::MouseEvent&)
{
    // STRICT QA requirement: one Undo transaction per drag GESTURE, begun here on
    // mouseDown, NOT one per mouseDrag tick — otherwise a single drag would flood
    // the UndoManager with hundreds of transactions (one per pixel of movement).
    workflow.getEdit().getUndoManager().beginNewTransaction ("Tweak Tempo");
    dragStartBpm = currentBpm();
}

void TransportBar::DraggableBpmLabel::mouseDrag (const juce::MouseEvent& e)
{
    const auto& tempos = workflow.getEdit().tempoSequence.getTempos();

    if (tempos.isEmpty())
        return;

    // Ableton convention: drag UP raises tempo, so invert the raw (downward-
    // positive) screen-Y delta.
    const double deltaBpm = -(double) e.getDistanceFromDragStartY() * 0.25;
    const double newBpm = juce::jlimit (20.0, 999.0, dragStartBpm + deltaBpm);
    tempos.getFirst()->setBpm (newBpm);
}

//==============================================================================
void TransportBar::MidiLed::setLit (bool shouldBeLit)
{
    if (lit == shouldBeLit)
        return;

    lit = shouldBeLit;
    repaint();
}

void TransportBar::MidiLed::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    const auto diameter = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto circle = bounds.withSizeKeepingCentre (diameter, diameter);

    g.setColour (lit ? LAF::ledOn : LAF::ledOff);
    g.fillEllipse (circle);
}

//==============================================================================
TransportBar::TransportBar (CrateWorkflowManager& workflowToUse)
    : workflow (workflowToUse)
{
    addAndMakeVisible (playButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (recordButton);
    addAndMakeVisible (loopButton);
    addAndMakeVisible (undoButton);
    addAndMakeVisible (redoButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (loadButton);
    addAndMakeVisible (pluginsButton);
    addAndMakeVisible (settingsButton);
    addAndMakeVisible (lcd);
    addAndMakeVisible (bpmLabel);
    addAndMakeVisible (cpuMeter);
    addAndMakeVisible (midiLed);
    addAndMakeVisible (toggleBrowserButton);
    addAndMakeVisible (toggleDeviceChainButton);
    addAndMakeVisible (timelineButton);
    addAndMakeVisible (mixerButton);

    loopButton.setClickingTogglesState (true);
    toggleBrowserButton.setClickingTogglesState (true);
    toggleDeviceChainButton.setClickingTogglesState (true);

    // Segmented pair: a shared non-zero radio group ID makes JUCE enforce
    // mutual exclusivity automatically on click (clicking one turns the other
    // off) — the correct built-in mechanism for exactly this "exactly one of
    // these two is ever active" case, rather than hand-rolling it. clickingTogglesState
    // is still required for radio-group behaviour to engage at all. The owner
    // (MainComponent) ALSO explicitly calls setToggleState() on both every time
    // the center view changes programmatically (Escape, MIDI Suite crossfade,
    // project Load) — the radio group only auto-updates on an actual click.
    constexpr int viewButtonRadioGroup = 1;
    timelineButton.setRadioGroupId (viewButtonRadioGroup);
    mixerButton.setRadioGroupId (viewButtonRadioGroup);
    timelineButton.setClickingTogglesState (true);
    mixerButton.setClickingTogglesState (true);

    // Drag-to-scrub affordance, matching Ableton's own BPM field cursor.
    bpmLabel.setMouseCursor (juce::MouseCursor::UpDownResizeCursor);

    // Routed through CrateWorkflowManager, not the transport directly — it's the
    // single source of truth for "where did the most recent play start" (so Stop
    // can rewind there, Ableton/Pro Tools style), shared with MainComponent's
    // Spacebar handler so the two never disagree about that position.
    playButton.onClick = [this] { workflow.startPlayback(); };
    stopButton.onClick = [this] { workflow.stopAndReturnToStart(); };

    recordButton.onClick = [this]
    {
        auto& transport = workflow.getEdit().getTransport();

        if (transport.isRecording())
            transport.stop (false, false);
        else
            transport.record (false, false);
    };

    loopButton.onClick = [this]
    {
        auto& transport = workflow.getEdit().getTransport();
        transport.looping = ! transport.looping;
        updateButtonStates();
    };

    undoButton.onClick = [this] { workflow.getEdit().getUndoManager().undo(); };
    redoButton.onClick = [this] { workflow.getEdit().getUndoManager().redo(); };

    saveButton.onClick = [this] { workflow.saveProject(); };

    // Just bubbles the click up — MainComponent calls workflow.safeLoadProject()
    // itself. See onLoadRequested's doc comment for why the load's completion
    // handling must NOT be routed back through a TransportBar member.
    loadButton.onClick = [this]
    {
        if (onLoadRequested)
            onLoadRequested();
    };

    settingsButton.onClick = [this]
    {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Audio & MIDI Settings";
        options.content.setOwned (new SettingsComponent (workflow.getEngine()));
        options.dialogBackgroundColour = juce::Colour (0xff202024);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;
        options.launchAsync();
    };

    pluginsButton.onClick = [this]
    {
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = "Plugin Manager";
        options.content.setOwned (new PluginBrowserComponent (workflow));
        options.dialogBackgroundColour = juce::Colour (0xff202024);
        options.escapeKeyTriggersCloseButton = true;
        options.useNativeTitleBar = true;
        options.resizable = true;

        // launchAsync() registers the dialog as modal (ModalComponentManager), and
        // TE's PluginWindowState::showWindow() refuses to open plugin editor windows
        // while any modal component is active — self-blocking as long as this browser
        // stays open. create() builds the same window without modal registration.
        // Tradeoff: unlike launchAsync(), this window isn't auto-deleted when closed
        // (that behaviour was tied to modal dismissal) — it hides on close and is
        // cleaned up on app exit. Acceptable for a dev tool opened occasionally;
        // revisit with explicit lifetime management if that becomes a real problem.
        auto* dialog = options.create();
        dialog->setVisible (true);
    };

    // MidiKeyChangeDispatcher notifies (lazily, on the message thread — see its own
    // doc comment in tracktion_MidiInputDevice.h) whenever any MIDI input plays a
    // note for any track. Registering/deregistering here, matched by the dtor below,
    // is the only listener this class owns.
    midiDispatcher->listeners.add (this);

    updateButtonStates();
    updateLcdAndStats();
    startTimerHz (20);
}

TransportBar::~TransportBar()
{
    stopTimer();
    midiDispatcher->listeners.remove (this);
}

void TransportBar::midiKeyStateChanged (te::AudioTrack*, const juce::Array<int>& notesOn,
                                         const juce::Array<int>&, const juce::Array<int>&)
{
    if (! notesOn.isEmpty())
        lastMidiActivityMs = juce::Time::getMillisecondCounter();
}

void TransportBar::updateButtonStates()
{
    auto& transport = workflow.getEdit().getTransport();

    playButton.setIconColour (transport.isPlaying() ? playActiveColour : idleIconColour);
    recordButton.setIconColour (transport.isRecording() ? recordActiveColour : recordIdleColour);
    loopButton.setToggleState (transport.looping, juce::dontSendNotification);
    loopButton.setIconColour (transport.looping ? juce::Colours::white : idleIconColour);

    undoButton.setEnabled (workflow.getEdit().getUndoManager().canUndo());
    redoButton.setEnabled (workflow.getEdit().getUndoManager().canRedo());
}

void TransportBar::updateLcdAndStats()
{
    auto& edit = workflow.getEdit();
    const auto position = edit.getTransport().getPosition();

    const auto barsBeats = edit.tempoSequence.toBarsAndBeats (position);
    const juce::String barsBeatsText = juce::String (barsBeats.bars + 1) + " . "
                                      + juce::String (barsBeats.getWholeBeats() + 1) + " . 1";

    const auto totalSeconds = position.inSeconds();
    const int hours = (int) (totalSeconds / 3600.0);
    const int mins  = (int) (std::fmod (totalSeconds, 3600.0) / 60.0);
    const int secs  = (int) std::fmod (totalSeconds, 60.0);
    const auto timeText = juce::String::formatted ("%02d:%02d:%02d", hours, mins, secs);

    lcd.setValues (barsBeatsText, timeText);

    const auto bpm = edit.tempoSequence.getBpmAt (position);
    bpmLabel.setDisplayText (juce::String (bpm, 1) + " BPM");

    cpuMeter.setLoad ((float) workflow.getEngine().getDeviceManager().getCpuUsage());

    const auto sinceActivity = (juce::int64) juce::Time::getMillisecondCounter() - lastMidiActivityMs;
    midiLed.setLit (lastMidiActivityMs != 0 && sinceActivity >= 0 && sinceActivity < midiActivityHoldMs);
}

void TransportBar::timerCallback()
{
    updateButtonStates();
    updateLcdAndStats();
}

void TransportBar::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);
    g.setColour (juce::Colour (0xff000000).withAlpha (0.6f));
    g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
}

void TransportBar::resized()
{
    auto area = getLocalBounds().reduced (8, 6);

    // Far right, per Four-Zone Shell spec — placed before (i.e. further right than)
    // the pre-existing Settings/Plugins buttons.
    toggleDeviceChainButton.setBounds (area.removeFromRight (90));
    area.removeFromRight (6);
    toggleBrowserButton.setBounds (area.removeFromRight (74));
    area.removeFromRight (12);

    // Segmented TIMELINE/MIXER pair — touching (no gap), so the shared edge
    // reads as one continuous two-state control rather than two separate
    // buttons. BUG FIX: the previous 56px slot truncated "ARRANGE" to
    // "ARRAN..." — 100px comfortably fits "TIMELINE" (the longer of the two
    // labels) at this button font size, with room to spare for "MIXER". Both
    // segments share the same width for a symmetric, sleeker look rather than
    // each shrink-wrapping to its own text.
    constexpr int viewButtonWidth = 100;
    auto viewButtons = area.removeFromRight (viewButtonWidth * 2);
    timelineButton.setBounds (viewButtons.removeFromLeft (viewButtonWidth));
    mixerButton.setBounds (viewButtons);
    area.removeFromRight (12);
    settingsButton.setBounds (area.removeFromRight (64));
    area.removeFromRight (6);
    pluginsButton.setBounds (area.removeFromRight (64));
    area.removeFromRight (12);
    saveButton.setBounds (area.removeFromRight (50));
    area.removeFromRight (6);
    loadButton.setBounds (area.removeFromRight (50));
    area.removeFromRight (14);

    // Vital stats cluster (BPM / CPU meter / MIDI LED), right of the LCD.
    auto vitalStats = area.removeFromRight (130);
    midiLed.setBounds (vitalStats.removeFromRight (10).withSizeKeepingCentre (8, 8));
    vitalStats.removeFromRight (8);
    cpuMeter.setBounds (vitalStats.removeFromRight (40).withSizeKeepingCentre (40, 10));
    vitalStats.removeFromRight (8);
    bpmLabel.setBounds (vitalStats);
    area.removeFromRight (14);

    // Left transport icon cluster.
    constexpr int iconSize = 30;
    playButton.setBounds (area.removeFromLeft (iconSize));
    area.removeFromLeft (4);
    stopButton.setBounds (area.removeFromLeft (iconSize));
    area.removeFromLeft (4);
    recordButton.setBounds (area.removeFromLeft (iconSize));
    area.removeFromLeft (4);
    loopButton.setBounds (area.removeFromLeft (iconSize));
    area.removeFromLeft (14);

    undoButton.setBounds (area.removeFromLeft (iconSize));
    area.removeFromLeft (4);
    redoButton.setBounds (area.removeFromLeft (iconSize));
    area.removeFromLeft (14);

    // LCD block: max 24px tall AND max 200px wide, centred in whatever space is
    // left in the middle — this is a compact status readout, not a hero
    // display. BUG FIX: this used to size to area.getWidth() unconditionally,
    // which meant the LCD stretched wider and wider as the window itself grew
    // (infinite stretching) instead of staying a fixed, tightly-grouped block.
    constexpr int lcdMaxHeight = 24;
    constexpr int lcdMaxWidth  = 200;
    lcd.setBounds (area.withSizeKeepingCentre (juce::jmin (lcdMaxWidth, area.getWidth()),
                                                juce::jmin (lcdMaxHeight, area.getHeight())));
}
