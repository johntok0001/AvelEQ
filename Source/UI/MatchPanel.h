#pragma once

#include "Theme.h"
#include "MatchEngine.h"

namespace avel::ui
{
    class MatchPanel : public juce::Component
    {
    public:
        MatchPanel (AvelProcessor& p, MatchEngine& e) : proc (p), engine (e)
        {
            auto setupLearn = [] (IconButton& b, const juce::String& tip)
            {
                b.setIcon (icon::record(), 1.8f);
                b.setText ("Learn");
                b.setBackgroundVisible (true);
                b.setToggleColour (col::red);
                b.setTooltip (tip);
            };
            setupLearn (learnRef, "Play the reference track through Avel EQ while this is on");
            setupLearn (learnCur, "Play your own track through Avel EQ while this is on");

            learnRef.onClick = [this]
            {
                engine.setLearning (engine.getLearning() == MatchEngine::Learn::reference ? MatchEngine::Learn::none : MatchEngine::Learn::reference);
            };
            learnCur.onClick = [this]
            {
                engine.setLearning (engine.getLearning() == MatchEngine::Learn::current ? MatchEngine::Learn::none : MatchEngine::Learn::current);
            };

            loadBtn.setIcon (icon::file(), 1.5f);
            loadBtn.setText ("Load File");
            loadBtn.setBackgroundVisible (true);
            loadBtn.setTooltip ("Use an audio file as the reference (you can also drag a file onto the plugin)");
            loadBtn.onClick = [this] { chooseFile(); };

            clearRef.setIcon (icon::close(), 1.6f);
            clearRef.setTooltip ("Forget the reference");
            clearRef.onClick = [this] { engine.clearReference(); };
            clearCur.setIcon (icon::close(), 1.6f);
            clearCur.setTooltip ("Forget your track");
            clearCur.onClick = [this] { engine.clearCurrent(); };

            closeBtn.setIcon (icon::close(), 1.6f);
            closeBtn.setTooltip ("Close Match EQ");
            closeBtn.onClick = [this] { if (onClose) onClose(); };

            amountDial.setFormatter ([] (float v) { return juce::String (juce::roundToInt (v)) + " %"; });
            amountDial.bindPlain ({ 0.0, 150.0, 1.0 }, engine.getAmount() * 100.0, 100.0, [this] (double v) { engine.setAmount (v / 100.0); });
            bandsDial.setFormatter ([] (float v) { return juce::String (juce::roundToInt (v)); });
            bandsDial.bindPlain ({ 1.0, 24.0, 1.0 }, engine.getBands(), 8.0, [this] (double v) { engine.setBands ((int) std::round (v)); });
            amountDial.setAccent (col::green);
            bandsDial.setAccent (col::green);

            smoothSeg.setItems ({ "1/12", "1/6", "1/3", "1/2", "1 oct" });
            smoothSeg.setTooltips ({ "Very detailed", "Detailed", "Balanced", "Smooth", "Very smooth" });
            smoothSeg.setSelected (engine.getSmoothing(), false);
            smoothSeg.onChange = [this] (int i) { engine.setSmoothing (i); };

            replaceSwitch.setOn (engine.getReplace(), false);
            replaceSwitch.setAccent (col::accent);
            replaceSwitch.onChange = [this] (bool b) { engine.setReplace (b); };

            applyBtn.setText ("Apply Match");
            applyBtn.setPrimary (col::green.darker (0.15f));
            applyBtn.onClick = [this] { engine.apply(); if (onApplied) onApplied(); };

            for (juce::Component* c : { (juce::Component*) &learnRef, (juce::Component*) &learnCur, (juce::Component*) &loadBtn,
                                        (juce::Component*) &clearRef, (juce::Component*) &clearCur, (juce::Component*) &closeBtn,
                                        (juce::Component*) &amountDial, (juce::Component*) &bandsDial, (juce::Component*) &smoothSeg,
                                        (juce::Component*) &replaceSwitch, (juce::Component*) &applyBtn })
                addAndMakeVisible (c);
        }

        std::function<void()> onClose, onApplied;

        void refresh()
        {
            learnRef.setToggled (engine.getLearning() == MatchEngine::Learn::reference);
            learnCur.setToggled (engine.getLearning() == MatchEngine::Learn::current);
            clearRef.setVisible (engine.hasReference());
            clearCur.setVisible (engine.hasCurrent());
            applyBtn.setEnabled (engine.canApply());
            const auto n = (int) engine.fittedBands().size();
            applyBtn.setText (engine.isFitting() ? "Analysing..." : n > 0 ? "Apply " + juce::String (n) + (n == 1 ? " Band" : " Bands") : "Apply Match");
            repaint (statusArea);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().reduced (16.0f, 4.0f).withTrimmedBottom (14.0f);
            drawGlass (g, r, 18.0f);

            // title
            g.setColour (col::green.withAlpha (0.25f));
            g.fillEllipse (juce::Rectangle<float> (18, 18).withCentre ({ (float) titleArea.getX() + 9, (float) titleArea.getCentreY() }));
            icon::draw (g, icon::match(), juce::Rectangle<float> (14, 14).withCentre ({ (float) titleArea.getX() + 9, (float) titleArea.getCentreY() }), col::green, 2.0f);
            g.setColour (col::label);
            g.setFont (font (13.5f, Weight::semibold));
            g.drawText ("Match EQ", titleArea.withTrimmedLeft (26), juce::Justification::centredLeft);
            g.setColour (col::label3);
            g.setFont (font (11.5f, Weight::medium));
            g.drawText (hint(), titleArea.withTrimmedLeft (110), juce::Justification::centredLeft, true);

            // column captions + status
            auto caption = [&] (juce::Rectangle<int> a, const juce::String& t, juce::Colour dot)
            {
                g.setColour (dot);
                g.fillEllipse (juce::Rectangle<float> (7, 7).withCentre ({ (float) a.getX() + 3.5f, (float) a.getCentreY() }));
                g.setColour (col::label3);
                g.setFont (font (10.0f, Weight::semibold, 0.06f));
                g.drawText (t, a.withTrimmedLeft (13), juce::Justification::centredLeft);
            };
            caption (refCaption, "REFERENCE", col::band (1));
            caption (curCaption, "YOUR TRACK", col::band (0));

            g.setFont (font (11.5f, Weight::medium));
            g.setColour (col::label2);
            g.drawText (refStatus(), refStatusArea, juce::Justification::centredLeft, true);
            g.drawText (curStatus(), curStatusArea, juce::Justification::centredLeft, true);

            g.setColour (col::stroke);
            for (int x : dividers)
                g.fillRect (juce::Rectangle<float> ((float) x, (float) bodyTop + 4, 1.0f, (float) bodyH - 8));

            g.setColour (col::label3);
            g.setFont (font (10.0f, Weight::semibold, 0.06f));
            g.drawText ("SMOOTHING", smoothCaption, juce::Justification::centredLeft);
            g.drawText ("REPLACE BANDS", replaceCaption, juce::Justification::centredLeft);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (16, 4).withTrimmedBottom (14).reduced (18, 12);
            auto top = r.removeFromTop (30);
            closeBtn.setBounds (top.removeFromRight (30));
            titleArea = top;
            r.removeFromTop (8);
            bodyTop = r.getY();
            bodyH = r.getHeight();
            dividers.clear();

            auto column = [&] (int w) { auto c = r.removeFromLeft (w); r.removeFromLeft (14); dividers.push_back (r.getX() - 7); return c; };

            auto c1 = column (190);
            refCaption = c1.removeFromTop (16);
            auto row = c1.removeFromTop (30);
            learnRef.setBounds (row.removeFromLeft (84));
            row.removeFromLeft (6);
            loadBtn.setBounds (row.removeFromLeft (100));
            auto st = c1.removeFromTop (24);
            clearRef.setBounds (st.removeFromRight (22).withSizeKeepingCentre (22, 22));
            refStatusArea = st;

            auto c2 = column (150);
            curCaption = c2.removeFromTop (16);
            row = c2.removeFromTop (30);
            learnCur.setBounds (row.removeFromLeft (84));
            st = c2.removeFromTop (24);
            clearCur.setBounds (st.removeFromRight (22).withSizeKeepingCentre (22, 22));
            curStatusArea = st;

            auto c3 = r.removeFromLeft (146);
            amountDial.setBounds (c3.removeFromLeft (72));
            bandsDial.setBounds (c3.removeFromLeft (72));
            r.removeFromLeft (14);

            auto c4 = r.removeFromLeft (juce::jmin (230, r.getWidth() - 150));
            smoothCaption = c4.removeFromTop (16);
            smoothSeg.setBounds (c4.removeFromTop (28));
            c4.removeFromTop (8);
            auto rep = c4.removeFromTop (24);
            replaceCaption = rep.removeFromLeft (110);
            replaceSwitch.setBounds (rep.removeFromLeft (44));
            r.removeFromLeft (14);

            applyBtn.setBounds (r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 150), 36));
            statusArea = getLocalBounds();
        }

    private:
        juce::String hint() const
        {
            if (! engine.hasReference()) return "1. Learn or load a reference";
            if (! engine.hasCurrent())   return "2. Learn your own track";
            return "3. Adjust and apply";
        }

        static juce::String secs (double s)
        {
            const int t = (int) std::round (s);
            return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
        }

        juce::String refStatus() const
        {
            if (engine.isLoadingFile()) return "Reading " + engine.referenceName() + "...";
            if (engine.getLearning() == MatchEngine::Learn::reference) return "Listening... " + secs (engine.referenceSeconds());
            if (! engine.hasReference()) return engine.referenceName().isNotEmpty() ? engine.referenceName() : "Not set";
            return engine.referenceName() + "  " + secs (engine.referenceSeconds());
        }

        juce::String curStatus() const
        {
            if (engine.getLearning() == MatchEngine::Learn::current) return "Listening... " + secs (engine.currentSeconds());
            if (! engine.hasCurrent()) return "Not set";
            return "Learned  " + secs (engine.currentSeconds());
        }

        void chooseFile()
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a reference track", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg;*.m4a;*.aac");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult() != juce::File()) engine.loadReferenceFile (fc.getResult()); });
        }

        AvelProcessor& proc;
        MatchEngine& engine;
        IconButton learnRef, learnCur, loadBtn, clearRef, clearCur, closeBtn, applyBtn;
        Dial amountDial { "Amount" }, bandsDial { "Bands" };
        Segmented smoothSeg;
        Switch replaceSwitch;
        std::unique_ptr<juce::FileChooser> chooser;

        juce::Rectangle<int> titleArea, refCaption, curCaption, refStatusArea, curStatusArea, smoothCaption, replaceCaption, statusArea;
        std::vector<int> dividers;
        int bodyTop = 0, bodyH = 0;
    };
}
