#include "PluginEditor.h"

using namespace avel;
using namespace avel::ui;

namespace
{
    constexpr int topBarH = 58;
    constexpr int panelH = 182;
}

AvelEditor::AvelEditor (AvelProcessor& p)
    : AudioProcessorEditor (&p), proc (p), matchEngine (p), graph (p), panel (p), matchPanel (p, matchEngine), meter (p)
{
    setLookAndFeel (&lnf);
    juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

    addAndMakeVisible (graph);
    addAndMakeVisible (meter);
    addChildComponent (panel);
    addChildComponent (matchPanel);

    graph.onMatchButton = [this] { setMatchOpen (! matchOpen); };
    matchPanel.onClose = [this] { setMatchOpen (false); };
    matchPanel.onApplied = [this] { setMatchOpen (false); };

    graph.onSelectionChanged = [this] { selectionChanged(); };
    panel.onDelete = [this] (int b)
    {
        proc.removeBand (b);
        graph.clearSelection();
    };
    panel.onStep = [this] (int dir)
    {
        const int start = juce::jmax (0, graph.getPrimary());
        for (int k = 1; k <= numBands; ++k)
        {
            const int b = ((start + dir * k) % numBands + numBands) % numBands;
            if (proc.band[(size_t) b].used->load() > 0.5f) { graph.selectOnly (b); break; }
        }
    };

    // top bar
    presetBtn.setIcon (icon::chevronDown(), 1.5f);
    presetBtn.setBackgroundVisible (true);
    presetBtn.setText (proc.currentPresetName);
    presetBtn.setTooltip ("Presets");
    presetBtn.onClick = [this] { showPresetMenu(); };
    addAndMakeVisible (presetBtn);

    undoBtn.setIcon (icon::undo(), 1.6f);
    redoBtn.setIcon (icon::redo(), 1.6f);
    undoBtn.setTooltip ("Undo");
    redoBtn.setTooltip ("Redo");
    undoBtn.onClick = [this] { proc.undo.undo(); };
    redoBtn.onClick = [this] { proc.undo.redo(); };
    addAndMakeVisible (undoBtn);
    addAndMakeVisible (redoBtn);

    phaseSeg.setItems ({ "Zero Latency", "Natural", "Linear" });
    phaseSeg.setTooltips ({ "Analog-matched filters, no latency",
                            "Natural Phase: analog magnitude and phase, small latency",
                            "Linear Phase: no phase shift at all, more latency" });
    phaseSeg.bind (proc.param ("phase"), &proc.undo);
    addAndMakeVisible (phaseSeg);

    resBtn.setIcon (icon::chevronDown(), 1.5f);
    resBtn.setBackgroundVisible (true);
    resBtn.setTooltip ("Linear phase resolution (higher = more precise low end, more latency)");
    resBtn.onClick = [this] { showResolutionMenu(); };
    addChildComponent (resBtn);

    abSeg.setItems ({ "A", "B" });
    abSeg.setTooltips ({ "Setting A", "Setting B" });
    abSeg.setSelected (proc.getABSlot(), false);
    abSeg.onChange = [this] (int i) { proc.switchAB (i); graph.clearSelection(); };
    addAndMakeVisible (abSeg);

    abCopyBtn.setIcon (icon::copy(), 1.5f);
    abCopyBtn.setTooltip ("Copy the current setting to the other slot");
    abCopyBtn.onClick = [this] { proc.copyABToOther(); };
    addAndMakeVisible (abCopyBtn);

    autoGainSwitch.bind (proc.param ("autoGain"), &proc.undo);
    autoGainSwitch.setAccent (col::accent);
    addAndMakeVisible (autoGainSwitch);

    setWantsKeyboardFocus (true);
    setResizable (true, true);
    setResizeLimits (980, 600, 2600, 1600);
    setSize ((int) proc.uiState.getProperty ("width", 1180), (int) proc.uiState.getProperty ("height", 720));

    startTimerHz (60);
}

AvelEditor::~AvelEditor()
{
    stopTimer();
    proc.setSoloBand (-1);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void AvelEditor::paint (juce::Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff141416), 0, 0, juce::Colour (0xff09090a), 0, b.getBottom(), false));
    g.fillAll();

    // logo: a soft prism
    auto l = logoArea.toFloat();
    auto mark = l.removeFromLeft (30).withSizeKeepingCentre (26, 26);
    juce::ColourGradient grad (juce::Colour (0xff64d2ff), mark.getX(), mark.getY(), juce::Colour (0xffbf5af2), mark.getRight(), mark.getBottom(), false);
    grad.addColour (0.5, juce::Colour (0xff0a84ff));
    g.setGradientFill (grad);
    g.fillRoundedRectangle (mark, 8.0f);
    juce::Path wave;
    wave.startNewSubPath (mark.getX() + 5, mark.getCentreY() + 2);
    wave.cubicTo (mark.getX() + 9, mark.getCentreY() + 2, mark.getX() + 10, mark.getY() + 7, mark.getCentreX(), mark.getY() + 7);
    wave.cubicTo (mark.getCentreX() + 3, mark.getY() + 7, mark.getRight() - 9, mark.getBottom() - 6, mark.getRight() - 5, mark.getBottom() - 6);
    g.setColour (juce::Colours::white);
    g.strokePath (wave, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    l.removeFromLeft (10);
    g.setColour (col::label);
    g.setFont (font (16.0f, Weight::semibold, -0.01f));
    g.drawText ("Avel", l, juce::Justification::centredLeft);
    const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), "Avel") + 5;
    g.setColour (col::label2);
    g.setFont (font (16.0f, Weight::regular, -0.01f));
    g.drawText ("EQ", l.withTrimmedLeft (w), juce::Justification::centredLeft);

    g.setColour (col::label3);
    g.setFont (font (10.0f, Weight::semibold, 0.06f));
    g.drawText ("AUTO GAIN", autoGainLabel, juce::Justification::centredRight);

    const float ag = proc.getAutoGainDb();
    if (autoGainSwitch.isOn())
    {
        g.setColour (col::label2);
        g.setFont (font (10.5f, Weight::medium));
        g.drawText (formatDb (ag), autoGainLabel.translated (0, 13), juce::Justification::centredRight);
    }
}

void AvelEditor::resized()
{
    proc.uiState.setProperty ("width", getWidth(), nullptr);
    proc.uiState.setProperty ("height", getHeight(), nullptr);

    auto r = getLocalBounds();
    auto top = r.removeFromTop (topBarH).reduced (18, 12);

    logoArea = top.removeFromLeft (104);
    top.removeFromLeft (14);
    presetBtn.setBounds (top.removeFromLeft (170).reduced (0, 2));
    top.removeFromLeft (10);
    undoBtn.setBounds (top.removeFromLeft (32).reduced (0, 1));
    top.removeFromLeft (2);
    redoBtn.setBounds (top.removeFromLeft (32).reduced (0, 1));

    auto right = top.removeFromRight (360);
    autoGainSwitch.setBounds (right.removeFromRight (44));
    autoGainLabel = right.removeFromRight (74).withTrimmedRight (6).withTrimmedTop (-2).withHeight (14).translated (0, 4);
    right.removeFromRight (20);
    abCopyBtn.setBounds (right.removeFromRight (32).reduced (0, 1));
    right.removeFromRight (4);
    abSeg.setBounds (right.removeFromRight (86).reduced (0, 1));

    auto centre = top.withSizeKeepingCentre (juce::jmin (top.getWidth(), 400), top.getHeight());
    resBtn.setBounds (centre.removeFromRight (142).reduced (0, 1));
    centre.removeFromRight (8);
    phaseSeg.setBounds (centre.reduced (0, 1));

    r = r.reduced (12, 0).withTrimmedBottom (12);
    meter.setBounds (r.removeFromRight (76));
    r.removeFromRight (6);
    graphArea = r;
    graph.setBounds (r);
    layoutPanel (false);
}

void AvelEditor::layoutPanel (bool)
{
    const int w = juce::jmin (graphArea.getWidth() - 20, 900);
    auto pb = juce::Rectangle<int> (w, panelH).withCentre ({ graphArea.getCentreX(), 0 });
    const int y = graphArea.getBottom() - 22 - panelH + juce::roundToInt ((1.0f - panelShown) * 30.0f);
    panel.setBounds (pb.withY (y));
    panel.setAlpha (panelShown);
    panel.setVisible (panelShown > 0.01f);

    const int my = graphArea.getBottom() - 22 - panelH + juce::roundToInt ((1.0f - matchShown) * 30.0f);
    matchPanel.setBounds (pb.withY (my));
    matchPanel.setAlpha (matchShown);
    matchPanel.setVisible (matchShown > 0.01f);
    graph.setBottomReserve (0.0f);
}

void AvelEditor::setMatchOpen (bool open)
{
    matchOpen = open;
    if (! open && matchEngine.getLearning() != MatchEngine::Learn::none)
        matchEngine.setLearning (MatchEngine::Learn::none);
    graph.setMatch (&matchEngine, open);
    if (open) graph.clearSelection();
}

bool AvelEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg;m4a;aac;caf"))
            return true;
    return false;
}

void AvelEditor::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;mp3;ogg;m4a;aac;caf"))
        {
            setMatchOpen (true);
            matchEngine.loadReferenceFile (juce::File (f));
            return;
        }
}

void AvelEditor::selectionChanged()
{
    const int b = graph.getPrimary();
    if (b >= 0)
    {
        panel.setBand (b);
        panelBand = b;
    }
}

void AvelEditor::timerCallback()
{
    matchEngine.tick();
    graph.tick();
    meter.tick();

    if (matchOpen && graph.getPrimary() >= 0) setMatchOpen (false);
    const bool want = graph.getPrimary() >= 0 && ! matchOpen;
    if (approach (matchShown, matchOpen ? 1.0f : 0.0f, 0.25f))
        layoutPanel (true);
    if (matchPanel.isVisible()) matchPanel.refresh();

    if (want && graph.getPrimary() != panel.getBand()) panel.setBand (graph.getPrimary());
    if (approach (panelShown, want ? 1.0f : 0.0f, 0.25f))
        layoutPanel (true);
    if (panel.isVisible()) panel.refresh();

    undoBtn.setEnabled (proc.undo.canUndo());
    redoBtn.setEnabled (proc.undo.canRedo());

    const bool lin = proc.isLinearPhase();
    if (resBtn.isVisible() != lin) resBtn.setVisible (lin);
    if (lin)
    {
        const char* names[] { "Low", "Medium", "High" };
        const int ri = juce::jlimit (0, 2, (int) proc.apvts.getRawParameterValue ("lpRes")->load());
        const int lat = proc.latencyForCurrentMode();
        juce::ignoreUnused (ri);
        const auto ms = juce::String (1000.0 * lat / juce::jmax (1.0, proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0), 0);
        resBtn.setText (juce::String (names[ri]) + "  " + ms + " ms");
    }

    if (presetBtn.isVisible()) presetBtn.setText (proc.currentPresetName);
    if (abSeg.getSelected() != proc.getABSlot()) abSeg.setSelected (proc.getABSlot());
    repaint (autoGainLabel.expanded (0, 16));
}

bool AvelEditor::keyPressed (const juce::KeyPress& k)
{
    return graph.keyPressed (k);
}

void AvelEditor::showPresetMenu()
{
    juce::PopupMenu m;
    const auto factory = AvelProcessor::getFactoryPresetNames();
    m.addSectionHeader ("Factory");
    for (int i = 0; i < factory.size(); ++i)
        m.addItem (1 + i, factory[i], true, proc.currentPresetName == factory[i]);

    juce::PopupMenu user;
    const auto files = AvelProcessor::getUserPresetFolder().findChildFiles (juce::File::findFiles, false, "*.avelpreset");
    juce::Array<juce::File> sorted (files);
    struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()); } } cmp;
    sorted.sort (cmp);
    for (int i = 0; i < sorted.size(); ++i)
        user.addItem (1000 + i, sorted[i].getFileNameWithoutExtension(), true, proc.currentPresetName == sorted[i].getFileNameWithoutExtension());
    if (sorted.isEmpty()) user.addItem (999, "No saved presets", false);

    m.addSeparator();
    m.addSubMenu ("My Presets", user);
    m.addSeparator();
    m.addItem (500, "Save Preset...");
    m.addItem (501, "Show Presets Folder");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presetBtn),
                     [this, sorted] (int r)
                     {
                         if (r >= 1 && r < 500) { proc.loadFactoryPreset (r - 1); graph.clearSelection(); }
                         else if (r >= 1000) { proc.loadUserPreset (sorted[r - 1000]); graph.clearSelection(); }
                         else if (r == 501) AvelProcessor::getUserPresetFolder().startAsProcess();
                         else if (r == 500)
                         {
                             chooser = std::make_unique<juce::FileChooser> ("Save preset",
                                         AvelProcessor::getUserPresetFolder().getChildFile (proc.currentPresetName + ".avelpreset"), "*.avelpreset");
                             chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                                                   [this] (const juce::FileChooser& fc)
                                                   {
                                                       auto f = fc.getResult();
                                                       if (f != juce::File()) proc.saveUserPreset (f.withFileExtension ("avelpreset"));
                                                   });
                         }
                     });
}

void AvelEditor::showResolutionMenu()
{
    juce::PopupMenu m;
    const int cur = (int) proc.apvts.getRawParameterValue ("lpRes")->load();
    m.addItem (1, "Low (4096 taps)", true, cur == 0);
    m.addItem (2, "Medium (16384 taps)", true, cur == 1);
    m.addItem (3, "High (32768 taps)", true, cur == 2);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&resBtn),
                     [this] (int r) { if (r > 0) { proc.undo.beginNewTransaction(); proc.setParam ("lpRes", (float) (r - 1)); } });
}
