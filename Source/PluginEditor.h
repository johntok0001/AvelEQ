#pragma once

#include "PluginProcessor.h"
#include "UI/Theme.h"
#include "UI/EQGraph.h"
#include "UI/BandPanel.h"
#include "UI/MatchEngine.h"
#include "UI/MatchPanel.h"

namespace avel::ui
{
    // Slim stereo output meter with output gain dial underneath
    class OutputMeter : public juce::Component
    {
    public:
        explicit OutputMeter (AvelProcessor& p) : proc (p)
        {
            gain.setFormatter ([] (float v) { return formatDb (v); });
            gain.bind (proc.param ("outGain"), &proc.undo);
            gain.setAccent (col::label);
            addAndMakeVisible (gain);
        }

        void tick()
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const float pk = proc.takeOutputPeak (ch);
                const float db = pk > 1.0e-6f ? 20.0f * std::log10 (pk) : -100.0f;
                level[ch] = db > level[ch] ? db : juce::jmax (db, level[ch] - 0.9f);
                if (db > hold[ch]) { hold[ch] = db; holdTime[ch] = 50; }
                else if (--holdTime[ch] < 0) hold[ch] = juce::jmax (-100.0f, hold[ch] - 0.6f);
                if (db > 0.0f) clip = 90;
            }
            if (clip > 0) --clip;
            repaint (meterArea);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = meterArea.toFloat();
            const float w = 5.0f, gap = 4.0f;
            auto toY = [&] (float db) { return r.getBottom() - juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f) * r.getHeight(); };

            g.setFont (font (9.5f, Weight::medium));
            for (float d : { 6.0f, 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f })
            {
                const float y = toY (d);
                g.setColour (col::label4);
                g.fillRect (juce::Rectangle<float> (r.getX() + 2 * w + gap + 4, y - 0.5f, 4, 1));
                g.setColour (d == 0.0f ? col::label2 : col::label3);
                g.drawText ((d > 0 ? "+" : "") + juce::String ((int) d), juce::Rectangle<float> (r.getX() + 2 * w + gap + 9, y - 6, 26, 12),
                            juce::Justification::centredLeft);
            }

            for (int ch = 0; ch < 2; ++ch)
            {
                auto bar = juce::Rectangle<float> (r.getX() + ch * (w + gap), r.getY(), w, r.getHeight());
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillRoundedRectangle (bar, w * 0.5f);
                const float y = toY (level[ch]);
                auto fillR = bar.withTop (y);
                g.setGradientFill (juce::ColourGradient (col::red, 0, toY (3.0f), col::green, 0, toY (-30.0f), false));
                g.fillRoundedRectangle (fillR, w * 0.5f);
                g.setColour (hold[ch] > 0.0f ? col::red : col::label);
                g.fillRect (juce::Rectangle<float> (bar.getX(), toY (hold[ch]) - 0.75f, w, 1.5f));
            }

            g.setColour (clip > 0 ? col::red : juce::Colours::white.withAlpha (0.1f));
            g.fillEllipse (juce::Rectangle<float> (6, 6).withCentre ({ r.getX() + w + gap * 0.5f, r.getY() - 10 }));

            g.setColour (col::label3);
            g.setFont (font (10.0f, Weight::semibold, 0.06f));
            g.drawText ("OUT", juce::Rectangle<float> (r.getX() - 10, r.getY() - 30, 2 * w + gap + 20, 12), juce::Justification::centred);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            gain.setBounds (r.removeFromBottom (86).reduced (0, 4));
            meterArea = r.withTrimmedTop (44).withTrimmedBottom (12).withSizeKeepingCentre (48, r.getHeight() - 56).withTrimmedRight (20);
        }

    private:
        AvelProcessor& proc;
        Dial gain { "Output", true };
        juce::Rectangle<int> meterArea;
        float level[2] { -100, -100 }, hold[2] { -100, -100 };
        int holdTime[2] { 0, 0 }, clip = 0;
    };
}

class AvelEditor : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit AvelEditor (AvelProcessor&);
    ~AvelEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    void setMatchOpen (bool open);
    bool isMatchOpen() const noexcept { return matchOpen; }
    avel::ui::MatchEngine& getMatchEngine() noexcept { return matchEngine; }
    avel::ui::EQGraph& getGraph() noexcept { return graph; }

private:
    void timerCallback() override;
    void selectionChanged();
    void showPresetMenu();
    void showResolutionMenu();
    void layoutPanel (bool animate);

    AvelProcessor& proc;
    avel::ui::LookAndFeel lnf;

    avel::ui::MatchEngine matchEngine;
    avel::ui::EQGraph graph;
    avel::ui::BandPanel panel;
    avel::ui::MatchPanel matchPanel;
    avel::ui::OutputMeter meter;

    avel::ui::IconButton presetBtn, undoBtn, redoBtn, abCopyBtn, resBtn;
    avel::ui::Segmented phaseSeg, abSeg;
    avel::ui::Switch autoGainSwitch;
    juce::Rectangle<int> logoArea, autoGainLabel, graphArea;

    std::unique_ptr<juce::FileChooser> chooser;
    juce::TooltipWindow tooltips { this, 600 };
    float panelShown = 0.0f, matchShown = 0.0f;
    bool matchOpen = false;
    int panelBand = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AvelEditor)
};
