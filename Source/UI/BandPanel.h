#pragma once

#include "Theme.h"
#include "../PluginProcessor.h"

namespace avel::ui
{
    // Floating glass card with all controls of the selected band
    class BandPanel : public juce::Component
    {
    public:
        explicit BandPanel (AvelProcessor& p) : proc (p)
        {
            typeSeg.setItems ({ "0", "1", "2", "3", "4", "5", "6", "7" }, true);
            typeSeg.setTooltips (typeNames());
            addAndMakeVisible (typeSeg);

            placeSeg.setItems ({ "Stereo", "L", "R", "M", "S" });
            placeSeg.setTooltips ({ "Stereo", "Left channel only", "Right channel only", "Mid (centre)", "Side (stereo width)" });
            addAndMakeVisible (placeSeg);

            soloBtn.setIcon (icon::headphones(), 1.5f);
            soloBtn.setTooltip ("Listen to this band");
            soloBtn.setToggleColour (col::yellow);
            soloBtn.onClick = [this] { proc.setSoloBand (proc.getSoloBand() == bandIndex ? -1 : bandIndex); };
            addAndMakeVisible (soloBtn);

            powerBtn.setIcon (icon::power(), 1.6f);
            powerBtn.setTooltip ("Enable / bypass band");
            powerBtn.onClick = [this]
            {
                proc.undo.beginNewTransaction ("Toggle band");
                proc.setParam (bandId (bandIndex, "on"), proc.band[(size_t) bandIndex].on->load() > 0.5f ? 0.0f : 1.0f);
            };
            addAndMakeVisible (powerBtn);

            deleteBtn.setIcon (icon::trash(), 1.5f);
            deleteBtn.setTooltip ("Delete band");
            deleteBtn.onClick = [this] { if (onDelete) onDelete (bandIndex); };
            addAndMakeVisible (deleteBtn);

            prevBtn.setIcon (icon::chevronLeft(), 1.7f);
            nextBtn.setIcon (icon::chevronRight(), 1.7f);
            prevBtn.setTooltip ("Previous band");
            nextBtn.setTooltip ("Next band");
            prevBtn.onClick = [this] { if (onStep) onStep (-1); };
            nextBtn.onClick = [this] { if (onStep) onStep (1); };
            addAndMakeVisible (prevBtn);
            addAndMakeVisible (nextBtn);

            slopeBtn.setIcon (icon::chevronDown(), 1.5f);
            slopeBtn.setBackgroundVisible (true);
            slopeBtn.setTooltip ("Slope");
            slopeBtn.onClick = [this] { showSlopeMenu(); };
            addAndMakeVisible (slopeBtn);

            freqDial.setFormatter ([] (float v) { return formatFreq (v); });
            gainDial.setFormatter ([] (float v) { return formatDb (v); });
            qDial.setFormatter ([] (float v) { return juce::String (v, v < 1.0f ? 3 : 2); });
            thrDial.setFormatter ([] (float v) { return juce::String (v, 1) + " dB"; });
            rangeDial.setFormatter ([] (float v) { return formatDb (v); });
            for (auto* d : { &freqDial, &gainDial, &qDial, &thrDial, &rangeDial })
                addAndMakeVisible (*d);

            dynSwitch.setAccent (col::green);
            addAndMakeVisible (dynSwitch);
        }

        std::function<void (int)> onDelete;
        std::function<void (int)> onStep;

        void setBand (int b)
        {
            if (b == bandIndex) { refresh(); return; }
            bandIndex = b;
            if (b < 0) return;

            auto* um = &proc.undo;
            typeSeg.bind (proc.param (bandId (b, "type")), um);
            placeSeg.bind (proc.param (bandId (b, "stereo")), um);
            freqDial.bind (proc.param (bandId (b, "freq")), um);
            gainDial.bind (proc.param (bandId (b, "gain")), um);
            qDial.bind (proc.param (bandId (b, "q")), um);
            thrDial.bind (proc.param (bandId (b, "thr")), um);
            rangeDial.bind (proc.param (bandId (b, "range")), um);
            dynSwitch.bind (proc.param (bandId (b, "dyn")), um);

            const auto c = col::band (b);
            for (auto* d : { &freqDial, &gainDial, &qDial, &thrDial, &rangeDial }) d->setAccent (c);
            typeSeg.setAccent (c);
            refresh();
            repaint();
        }

        int getBand() const noexcept { return bandIndex; }

        // cheap, called every frame
        void refresh()
        {
            if (bandIndex < 0) return;
            const auto& p = proc.band[(size_t) bandIndex];
            const auto t = (FilterType) (int) p.type->load();
            const bool gainType = typeHasGain (t);
            const bool on = p.on->load() > 0.5f;
            const bool dyn = p.dyn->load() > 0.5f;

            gainDial.setDimmed (! gainType || ! on);
            gainDial.setEnabled (gainType);
            freqDial.setDimmed (! on);
            qDial.setDimmed (! on);

            if (slopeBtn.isVisible() != typeHasSlope (t)) { slopeBtn.setVisible (typeHasSlope (t)); }
            slopeBtn.setText (juce::String (slopeChoices[(size_t) juce::jlimit (0, 8, (int) p.slope->load())]) + " dB/oct");

            dynSwitch.setEnabled (gainType);
            thrDial.setDimmed (! (gainType && dyn));
            rangeDial.setDimmed (! (gainType && dyn));
            thrDial.setEnabled (gainType && dyn);
            rangeDial.setEnabled (gainType && dyn);

            powerBtn.setToggled (on);
            powerBtn.setToggleColour (col::band (bandIndex));
            soloBtn.setToggled (proc.getSoloBand() == bandIndex);

            if (lastOn != on || lastType != (int) t) { lastOn = on; lastType = (int) t; resized(); repaint(); }
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (16.0f, 4.0f).withTrimmedBottom (14.0f);
            drawGlass (g, r, 18.0f);

            if (bandIndex < 0) return;
            const auto c = col::band (bandIndex);

            // header
            auto head = nameArea.toFloat();
            g.setColour (c.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (18, 18).withCentre ({ head.getX() + 9, head.getCentreY() }));
            g.setColour (c);
            g.fillEllipse (juce::Rectangle<float> (10, 10).withCentre ({ head.getX() + 9, head.getCentreY() }));
            g.setColour (col::label);
            g.setFont (font (13.5f, Weight::semibold));
            g.drawText ("Band " + juce::String (bandIndex + 1), head.withTrimmedLeft (24), juce::Justification::centredLeft);

            // section divider + dynamic caption
            g.setColour (col::stroke);
            g.fillRect (juce::Rectangle<float> ((float) dividerX, (float) knobRow.getY() + 6, 1.0f, (float) knobRow.getHeight() - 12));

            g.setColour (col::label3);
            g.setFont (font (10.0f, Weight::semibold, 0.06f));
            g.drawText ("DYNAMIC", dynLabel, juce::Justification::centred);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16, 4).withTrimmedBottom (14).reduced (18, 12);

            auto top = r.removeFromTop (30);
            nameArea = top.removeFromLeft (96);
            prevBtn.setBounds (top.removeFromLeft (26).reduced (0, 2));
            nextBtn.setBounds (top.removeFromLeft (26).reduced (0, 2));
            top.removeFromLeft (12);
            typeSeg.setBounds (top.removeFromLeft (juce::jmin (300, top.getWidth() / 2)));

            deleteBtn.setBounds (top.removeFromRight (30));
            top.removeFromRight (4);
            powerBtn.setBounds (top.removeFromRight (30));
            top.removeFromRight (4);
            soloBtn.setBounds (top.removeFromRight (30));
            top.removeFromRight (14);
            placeSeg.setBounds (top.removeFromRight (juce::jmin (210, top.getWidth() - 10)));

            r.removeFromTop (10);
            knobRow = r;

            const int kw = 72;
            auto k = r;
            freqDial.setBounds (k.removeFromLeft (kw));
            gainDial.setBounds (k.removeFromLeft (kw));
            qDial.setBounds (k.removeFromLeft (kw));

            if (slopeBtn.isVisible())
            {
                k.removeFromLeft (8);
                slopeBtn.setBounds (k.removeFromLeft (104).withSizeKeepingCentre (104, 28).translated (0, -4));
            }

            auto dyn = r.removeFromRight (kw * 2 + 70);
            dividerX = dyn.getX() - 14;
            auto sw = dyn.removeFromLeft (64);
            dynLabel = sw.removeFromTop (14).toFloat();
            dynSwitch.setBounds (sw.withSizeKeepingCentre (44, 30).translated (0, -6));
            thrDial.setBounds (dyn.removeFromLeft (kw));
            rangeDial.setBounds (dyn.removeFromLeft (kw));
        }

    private:
        void showSlopeMenu()
        {
            juce::PopupMenu m;
            const int cur = (int) proc.band[(size_t) bandIndex].slope->load();
            for (int i = 0; i < (int) slopeChoices.size(); ++i)
                m.addItem (i + 1, juce::String (slopeChoices[(size_t) i]) + " dB/oct", true, i == cur);
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&slopeBtn),
                             [this] (int r)
                             {
                                 if (r <= 0 || bandIndex < 0) return;
                                 proc.undo.beginNewTransaction ("Slope");
                                 proc.setParam (bandId (bandIndex, "slope"), (float) (r - 1));
                             });
        }

        AvelProcessor& proc;
        int bandIndex = -1, lastType = -1;
        bool lastOn = true;

        Segmented typeSeg, placeSeg;
        IconButton soloBtn, powerBtn, deleteBtn, prevBtn, nextBtn, slopeBtn;
        Dial freqDial { "Frequency" }, gainDial { "Gain", true }, qDial { "Q" }, thrDial { "Threshold" }, rangeDial { "Range", true };
        Switch dynSwitch;

        juce::Rectangle<int> nameArea, knobRow;
        juce::Rectangle<float> dynLabel;
        int dividerX = 0;
    };
}
