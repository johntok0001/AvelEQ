#pragma once

#include <juce_dsp/juce_dsp.h>
#include "Theme.h"
#include "MatchEngine.h"
#include "../PluginProcessor.h"
#include "../DSP/CurveFit.h"

namespace avel::ui
{
    class EQGraph : public juce::Component
    {
    public:
        explicit EQGraph (AvelProcessor& p);
        ~EQGraph() override;

        // called by the editor at 60 Hz
        void tick();

        // selection
        int getPrimary() const noexcept { return primary; }
        void selectOnly (int b);
        void clearSelection();
        std::function<void()> onSelectionChanged;

        // Match EQ overlay
        void setMatch (MatchEngine* m, bool open) { match = m; matchOpen = open; matchBtn.setToggled (open); repaint(); }
        std::function<void()> onMatchButton;

        // EQ Sketch
        void setSketchMode (bool on);
        bool isSketchMode() const noexcept { return sketchMode; }

        // for tests / automation
        bool isGrabActive() const noexcept { return grabActive; }
        void activateGrabAt (juce::Point<float> p);
        int getNumPeaks() const noexcept { return (int) peaks.size(); }
        double getPeakFreq (int i) const noexcept { return peaks[(size_t) i].f; }

        void setBottomReserve (float px) { bottomReserve = px; repaint(); }

        void paint (juce::Graphics&) override;
        void resized() override;

        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        bool keyPressed (const juce::KeyPress&) override;

    private:
        static constexpr double fMin = 10.0, fMax = 30000.0;

        // mapping
        float xForFreq (double f) const noexcept;
        double freqForX (float x) const noexcept;
        float yForDb (double db) const noexcept;
        double dbForY (float y) const noexcept;
        float yForSpectrum (double db) const noexcept;
        double uiSampleRate() const noexcept;

        // bands
        bool isUsed (int b) const noexcept { return proc.band[(size_t) b].used->load() > 0.5f; }
        juce::Point<float> nodePos (int b) const noexcept;
        double nodeDisplayDb (int b) const noexcept;
        int nodeAt (juce::Point<float> p) const noexcept;
        void setBandParam (int b, const char* id, float v);
        void beginGesture (int b);
        void endGesture (int b);
        int createBand (FilterType t, double f, double gain, float q = -1.0f);

        // drawing helpers
        void rebuildCurves();
        void updateSpectrum();
        void drawGrid (juce::Graphics&);
        void drawSpectrum (juce::Graphics&);
        void drawBands (juce::Graphics&);
        void drawNodes (juce::Graphics&);
        void drawInfo (juce::Graphics&);
        void drawMatch (juce::Graphics&);
        void drawGrab (juce::Graphics&);
        void drawSketch (juce::Graphics&);
        juce::Path curvePath (const std::vector<float>& db) const;
        void drawPill (juce::Graphics&, juce::Point<float> centre, const juce::String& text, juce::Colour dot = {}) const;

        // analyzer
        void setAnalyzerResolution (int r);
        void showNodeMenu (int b);
        void showAddMenu (juce::Point<float> at);
        void showAnalyzerMenu();
        void selectionChanged();
        void saveAnalyzerPrefs();

        // spectrum grab
        void computeGrab();
        void endGrab();
        int peakAt (juce::Point<float> p) const noexcept;

        // sketch
        void finishSketch();

        AvelProcessor& proc;
        juce::Rectangle<float> plot;
        float bottomReserve = 0.0f;
        double rangeDb = 12.0;
        int analyzerMode = 0;   // 0 pre+post, 1 post, 2 pre, 3 off
        int analyzerRes = 1;    // 0..3 -> FFT 4096..32768
        int analyzerSpeed = 1;  // slow / medium / fast
        double tiltDb = 4.5;
        bool peakHold = false, grabEnabled = true;

        // curves
        int numPts = 0;
        std::vector<float> ptFreq, total;
        std::array<std::vector<float>, numBands> bandDb;
        std::array<bool, numBands> bandShown {};
        uint64_t curveHash = 0;

        // analyzer
        int fftOrder = 13, fftSize = 1 << 13;
        std::unique_ptr<juce::dsp::FFT> fft;
        std::unique_ptr<juce::dsp::WindowingFunction<float>> window;
        std::vector<float> fftBuf, scopeTmp, preDb, postDb, holdDb;
        std::vector<double> prefix;

        // spectrum grab
        struct Peak { double f, db, q, prom; };
        bool grabActive = false;
        double armTime = 0.0;
        juce::Point<float> armPos { -100, -100 };
        std::vector<double> grabFreq, grabDb;
        std::vector<Peak> peaks;
        int hoverPeak = -1;

        // sketch
        bool sketchMode = false, sketchFitting = false;
        std::vector<juce::Point<float>> sketchPts;
        juce::ThreadPool fitPool { 1 };
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

        // match overlay
        MatchEngine* match = nullptr;
        bool matchOpen = false;

        // animation
        std::array<float, numBands> hoverAmt {}, selAmt {}, appear {};
        float grabAmt = 0.0f;

        // interaction
        enum class Drag { none, node, lasso, sketch };
        Drag dragMode = Drag::none;
        juce::Array<int> selection;
        int primary = -1, hoverNode = -1;
        float ghostX = -1.0f;
        juce::Point<float> mousePos { -1, -1 };
        bool mouseInside = false, dragMoved = false, shiftToggleCandidate = false;
        juce::Rectangle<float> lasso;
        juce::Array<int> lassoBase;

        struct DragStart { int band; double freq, gain, q; };
        std::vector<DragStart> dragStarts;
        juce::uint32 lastWheelTime = 0;

        Segmented rangeSeg;
        IconButton analyzerButton, matchBtn, sketchBtn;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EQGraph)
    };
}
