#include "EQGraph.h"

namespace avel::ui
{
    namespace
    {
        constexpr float nodeRadius = 6.5f;

        double cutDisplayDb (double q, double base) { return 6.0 * std::log2 (q / base); }
        double cutQFromDisplay (double db, double base) { return base * std::pow (2.0, db / 6.0); }

        double qBaseFor (FilterType t)
        {
            return (t == FilterType::lowCut || t == FilterType::highCut) ? 0.7071067811865476 : 1.0;
        }

        juce::String noteName (double f)
        {
            static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            int note = 0, cents = 0;
            noteForFrequency (f, note, cents);
            const int octave = note / 12 - 1;
            juce::String s = juce::String (names[((note % 12) + 12) % 12]) + juce::String (octave);
            if (cents != 0) s << (cents > 0 ? " +" : " ") << cents << juce::String (juce::CharPointer_UTF8 ("\xc2\xa2"));
            return s;
        }
    }

    EQGraph::EQGraph (AvelProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (true);
        setOpaque (false);

        rangeDb = (double) (int) proc.uiState.getProperty ("range", 12);
        analyzerMode = (int) proc.uiState.getProperty ("analyzer", 0);
        analyzerSpeed = (int) proc.uiState.getProperty ("anSpeed", 1);
        tiltDb = (double) proc.uiState.getProperty ("anTilt", 4.5);
        peakHold = (bool) proc.uiState.getProperty ("anHold", false);
        grabEnabled = (bool) proc.uiState.getProperty ("anGrab", true);
        setAnalyzerResolution ((int) proc.uiState.getProperty ("anRes", 1));

        rangeSeg.setItems ({ "3", "6", "12", "30" });
        rangeSeg.setTooltips ({ "Display range +/-3 dB", "Display range +/-6 dB", "Display range +/-12 dB", "Display range +/-30 dB" });
        const int ri = rangeDb <= 3 ? 0 : rangeDb <= 6 ? 1 : rangeDb <= 12 ? 2 : 3;
        rangeSeg.setSelected (ri, false);
        rangeSeg.onChange = [this] (int i)
        {
            static const double r[] { 3.0, 6.0, 12.0, 30.0 };
            rangeDb = r[i];
            proc.uiState.setProperty ("range", (int) rangeDb, nullptr);
            curveHash = 0;
            repaint();
        };
        addAndMakeVisible (rangeSeg);

        analyzerButton.setIcon (icon::chevronDown(), 1.6f);
        analyzerButton.setBackgroundVisible (true);
        {
            const char* labels[] { "Analyzer: Pre+Post", "Analyzer: Post", "Analyzer: Pre", "Analyzer: Off" };
            analyzerButton.setText (labels[juce::jlimit (0, 3, analyzerMode)]);
        }
        analyzerButton.onClick = [this] { showAnalyzerMenu(); };
        analyzerButton.setTooltip ("Spectrum analyzer");
        addAndMakeVisible (analyzerButton);

        matchBtn.setIcon (icon::match(), 1.6f);
        matchBtn.setText ("Match EQ");
        matchBtn.setBackgroundVisible (true);
        matchBtn.setToggleColour (col::green);
        matchBtn.setTooltip ("Match the tonal balance of a reference track");
        matchBtn.onClick = [this] { if (onMatchButton) onMatchButton(); };
        addAndMakeVisible (matchBtn);

        sketchBtn.setIcon (icon::pencil(), 1.5f);
        sketchBtn.setText ("Sketch");
        sketchBtn.setBackgroundVisible (true);
        sketchBtn.setToggleColour (col::accent);
        sketchBtn.setTooltip ("Draw the curve you want, Avel EQ creates the bands");
        sketchBtn.onClick = [this] { setSketchMode (! sketchMode); };
        addAndMakeVisible (sketchBtn);

        for (auto& a : appear) a = 1.0f;
    }

    EQGraph::~EQGraph()
    {
        *alive = false;
        fitPool.removeAllJobs (true, 3000);
    }

    void EQGraph::setAnalyzerResolution (int r)
    {
        analyzerRes = juce::jlimit (0, 3, r);
        fftOrder = 12 + analyzerRes;
        fftSize = 1 << fftOrder;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);
        window = std::make_unique<juce::dsp::WindowingFunction<float>> ((size_t) fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris, false);
        fftBuf.assign ((size_t) fftSize * 2, 0.0f);
        scopeTmp.assign ((size_t) fftSize, 0.0f);
        prefix.assign ((size_t) fftSize / 2 + 2, 0.0);
    }

    void EQGraph::saveAnalyzerPrefs()
    {
        proc.uiState.setProperty ("analyzer", analyzerMode, nullptr);
        proc.uiState.setProperty ("anRes", analyzerRes, nullptr);
        proc.uiState.setProperty ("anSpeed", analyzerSpeed, nullptr);
        proc.uiState.setProperty ("anTilt", tiltDb, nullptr);
        proc.uiState.setProperty ("anHold", peakHold, nullptr);
        proc.uiState.setProperty ("anGrab", grabEnabled, nullptr);
    }

    void EQGraph::setSketchMode (bool on)
    {
        sketchMode = on;
        sketchBtn.setToggled (on);
        if (! on) { sketchPts.clear(); if (dragMode == Drag::sketch) dragMode = Drag::none; }
        endGrab();
        setMouseCursor (on ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    //==============================================================================
    double EQGraph::uiSampleRate() const noexcept
    {
        const double sr = proc.getSampleRate();
        return sr > 0 ? sr : 48000.0;
    }

    float EQGraph::xForFreq (double f) const noexcept
    {
        return plot.getX() + plot.getWidth() * (float) (std::log (f / fMin) / std::log (fMax / fMin));
    }

    double EQGraph::freqForX (float x) const noexcept
    {
        return fMin * std::pow (fMax / fMin, (double) ((x - plot.getX()) / plot.getWidth()));
    }

    float EQGraph::yForDb (double db) const noexcept
    {
        const float mid = plot.getY() + (plot.getHeight() - bottomReserve) * 0.5f;
        const float half = (plot.getHeight() - bottomReserve) * 0.5f * 0.82f;
        return mid - (float) (db / rangeDb) * half;
    }

    double EQGraph::dbForY (float y) const noexcept
    {
        const float mid = plot.getY() + (plot.getHeight() - bottomReserve) * 0.5f;
        const float half = (plot.getHeight() - bottomReserve) * 0.5f * 0.82f;
        return (double) ((mid - y) / half) * rangeDb;
    }

    float EQGraph::yForSpectrum (double db) const noexcept
    {
        // -96 dB at the bottom of the plot, +12 dB at the top
        return plot.getBottom() - (float) ((db + 96.0) / 108.0) * plot.getHeight();
    }

    void EQGraph::resized()
    {
        plot = getLocalBounds().toFloat().reduced (1.0f).withTrimmedBottom (22.0f);
        numPts = juce::jmax (16, (int) plot.getWidth() / 2);
        ptFreq.resize ((size_t) numPts);
        for (int i = 0; i < numPts; ++i)
            ptFreq[(size_t) i] = (float) freqForX (plot.getX() + plot.getWidth() * i / (numPts - 1));
        total.assign ((size_t) numPts, 0.0f);
        for (auto& b : bandDb) b.assign ((size_t) numPts, 0.0f);
        preDb.assign ((size_t) numPts, -120.0f);
        postDb.assign ((size_t) numPts, -120.0f);
        holdDb.assign ((size_t) numPts, -120.0f);
        curveHash = 0;
        endGrab();

        auto tools = getLocalBounds().reduced (14, 12).removeFromTop (26);
        matchBtn.setBounds (tools.removeFromLeft (108));
        tools.removeFromLeft (6);
        sketchBtn.setBounds (tools.removeFromLeft (90));

        auto top = getLocalBounds().reduced (14, 12).removeFromTop (26);
        analyzerButton.setBounds (top.removeFromRight (156));
        top.removeFromRight (8);
        rangeSeg.setBounds (top.removeFromRight (132));
    }

    //==============================================================================
    juce::Point<float> EQGraph::nodePos (int b) const noexcept
    {
        const auto& p = proc.band[(size_t) b];
        return { xForFreq (juce::jlimit (fMin, fMax, (double) p.freq->load())), yForDb (juce::jlimit (-rangeDb, rangeDb, nodeDisplayDb (b))) };
    }

    double EQGraph::nodeDisplayDb (int b) const noexcept
    {
        const auto& p = proc.band[(size_t) b];
        const auto t = (FilterType) (int) p.type->load();
        if (typeHasGain (t))
            return p.gain->load();
        return cutDisplayDb (p.q->load(), qBaseFor (t));
    }

    int EQGraph::nodeAt (juce::Point<float> pt) const noexcept
    {
        int best = -1;
        float bestD = 11.0f;
        // prefer the selected band when nodes overlap
        for (int b = numBands - 1; b >= 0; --b)
        {
            if (! isUsed (b)) continue;
            float d = nodePos (b).getDistanceFrom (pt);
            if (selection.contains (b)) d -= 2.0f;
            if (d < bestD) { bestD = d; best = b; }
        }
        return best;
    }

    void EQGraph::setBandParam (int b, const char* id, float v)
    {
        if (auto* prm = proc.param (bandId (b, id)))
            prm->setValueNotifyingHost (prm->convertTo0to1 (v));
    }

    void EQGraph::beginGesture (int b)
    {
        for (const char* id : { "freq", "gain", "q" })
            if (auto* prm = proc.param (bandId (b, id))) prm->beginChangeGesture();
    }

    void EQGraph::endGesture (int b)
    {
        for (const char* id : { "freq", "gain", "q" })
            if (auto* prm = proc.param (bandId (b, id))) prm->endChangeGesture();
    }

    int EQGraph::createBand (FilterType t, double f, double gain, float qIn)
    {
        const float q = qIn > 0.0f ? qIn : (t == FilterType::lowCut || t == FilterType::highCut || t == FilterType::lowShelf || t == FilterType::highShelf) ? 0.7071f : 1.0f;
        const int b = proc.addBand (t, (float) juce::jlimit (fMin, fMax, f), (float) juce::jlimit (-30.0, 30.0, gain), q);
        if (b >= 0)
        {
            appear[(size_t) b] = 0.0f;
            selectOnly (b);
        }
        return b;
    }

    //==============================================================================
    void EQGraph::selectOnly (int b)
    {
        selection.clearQuick();
        if (b >= 0) selection.add (b);
        primary = b;
        selectionChanged();
    }

    void EQGraph::clearSelection()
    {
        selection.clearQuick();
        primary = -1;
        selectionChanged();
    }

    void EQGraph::selectionChanged()
    {
        repaint();
        if (onSelectionChanged) onSelectionChanged();
    }

    //==============================================================================
    void EQGraph::tick()
    {
        // drop bands that disappeared (undo, preset)
        bool changed = false;
        for (int i = selection.size(); --i >= 0;)
            if (! isUsed (selection[i])) { selection.remove (i); changed = true; }
        if (primary >= 0 && ! isUsed (primary)) { primary = selection.isEmpty() ? -1 : selection.getLast(); changed = true; }
        if (changed) selectionChanged();

        for (int b = 0; b < numBands; ++b)
        {
            approach (hoverAmt[(size_t) b], (b == hoverNode || (dragMode == Drag::node && selection.contains (b))) ? 1.0f : 0.0f, 0.3f);
            approach (selAmt[(size_t) b], selection.contains (b) ? 1.0f : 0.0f, 0.3f);
            approach (appear[(size_t) b], 1.0f, 0.18f);
        }

        // Spectrum Grab: hover still over the analyzer for a moment
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (! grabActive && grabEnabled && analyzerMode != 3 && ! sketchMode && dragMode == Drag::none && mouseInside
            && hoverNode < 0 && ghostX < 0.0f && armTime > 0.0 && now - armTime > 550.0 && plot.contains (mousePos)
            && mousePos.getDistanceFrom (armPos) < 10.0f && ! juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        {
            computeGrab();
            grabActive = true;
            hoverPeak = peakAt (mousePos);
        }
        approach (grabAmt, grabActive ? 1.0f : 0.0f, 0.2f);

        rebuildCurves();
        if (analyzerMode != 3 && ! grabActive)
            updateSpectrum();
        repaint();
    }

    void EQGraph::rebuildCurves()
    {
        if (numPts <= 0) return;

        uint64_t h = 1469598103934665603ull;
        auto mix = [&h] (float v) { uint32_t bits; std::memcpy (&bits, &v, 4); h = (h ^ bits) * 1099511628211ull; };
        for (int b = 0; b < numBands; ++b)
        {
            const auto& p = proc.band[(size_t) b];
            mix (p.used->load()); mix (p.on->load());
            if (p.used->load() < 0.5f) continue;
            mix (p.type->load()); mix (p.freq->load()); mix (p.gain->load()); mix (p.q->load()); mix (p.slope->load());
            mix (p.dyn->load()); mix (p.range->load());
            if (proc.isDynamicActive (b)) mix (proc.getDynamicGain (b));
        }
        mix ((float) uiSampleRate());
        mix ((float) rangeDb);
        if (h == curveHash) return;
        curveHash = h;

        const double sr = uiSampleRate();
        std::fill (total.begin(), total.end(), 0.0f);

        for (int b = 0; b < numBands; ++b)
        {
            const auto& p = proc.band[(size_t) b];
            bandShown[(size_t) b] = p.used->load() > 0.5f;
            if (! bandShown[(size_t) b]) continue;

            const bool dynLive = proc.isDynamicActive (b);
            const auto design = designBand (p.settings (dynLive ? proc.getDynamicGain (b) : std::numeric_limits<float>::quiet_NaN()), sr);
            const bool on = p.on->load() > 0.5f;
            auto& d = bandDb[(size_t) b];

            for (int i = 0; i < numPts; ++i)
            {
                const double f = ptFreq[(size_t) i];
                const float v = f < 0.5 * sr ? (float) design.magDb (2.0 * pi * f / sr) : d[(size_t) juce::jmax (0, i - 1)];
                d[(size_t) i] = v;
                if (on) total[(size_t) i] += v;
            }
        }
    }

    void EQGraph::updateSpectrum()
    {
        const double sr = uiSampleRate();
        const int half = fftSize / 2;
        const double binHz = sr / fftSize;

        auto analyse = [&] (const ScopeRing& ring, std::vector<float>& out)
        {
            ring.readLatest (scopeTmp.data(), fftSize);
            std::copy (scopeTmp.begin(), scopeTmp.end(), fftBuf.begin());
            std::fill (fftBuf.begin() + fftSize, fftBuf.end(), 0.0f);
            window->multiplyWithWindowingTable (fftBuf.data(), (size_t) fftSize);
            fft->performFrequencyOnlyForwardTransform (fftBuf.data(), true);

            const double norm = 2.0 / (fftSize * 0.3587); // Blackman-Harris coherent gain
            prefix[0] = 0.0;
            for (int k = 0; k <= half; ++k)
            {
                const double a = fftBuf[(size_t) k] * norm;
                prefix[(size_t) k + 1] = prefix[(size_t) k] + a * a;
            }

            const double bw = std::pow (2.0, 1.0 / 12.0);
            for (int i = 0; i < numPts; ++i)
            {
                const double f = ptFreq[(size_t) i];
                double lo = f / bw / binHz, hi = f * bw / binHz;
                if (hi - lo < 1.0) { const double c = 0.5 * (lo + hi); lo = c - 0.5; hi = c + 0.5; }
                auto cum = [&] (double bin)
                {
                    bin = juce::jlimit (0.0, (double) half, bin + 0.5);
                    const int i0 = (int) bin;
                    return prefix[(size_t) i0] + (prefix[(size_t) juce::jmin (i0 + 1, half + 1)] - prefix[(size_t) i0]) * (bin - i0);
                };
                const double pw = juce::jmax (1.0e-14, (cum (hi) - cum (lo)) / (hi - lo));
                const double tilt = tiltDb * std::log2 (f / 1000.0);
                const float db = f < 0.5 * sr ? (float) (10.0 * std::log10 (pw) + tilt) : -120.0f;
                float& v = out[(size_t) i];
                static const float fall[] { 0.05f, 0.12f, 0.3f };
                v += (db - v) * (db > v ? 0.6f : fall[juce::jlimit (0, 2, analyzerSpeed)]);
            }
        };

        if (analyzerMode == 0 || analyzerMode == 2) analyse (proc.preScope, preDb);
        if (analyzerMode == 0 || analyzerMode == 1) analyse (proc.postScope, postDb);

        if (peakHold)
        {
            const auto& src = analyzerMode == 2 ? preDb : postDb;
            for (int i = 0; i < numPts; ++i)
                holdDb[(size_t) i] = juce::jmax (holdDb[(size_t) i], src[(size_t) i]);
        }
    }

    //==============================================================================
    juce::Path EQGraph::curvePath (const std::vector<float>& db) const
    {
        juce::Path p;
        for (int i = 0; i < numPts; ++i)
        {
            const float x = plot.getX() + plot.getWidth() * i / (numPts - 1);
            const float y = juce::jlimit (plot.getY() - 40.0f, plot.getBottom() + 40.0f, yForDb (db[(size_t) i]));
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        return p;
    }

    void EQGraph::paint (juce::Graphics& g)
    {
        auto bounds = getLocalBounds().toFloat();
        const float radius = 14.0f;

        // body
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff17171a), bounds.getX(), bounds.getY(),
                                                 juce::Colour (0xff0c0c0e), bounds.getX(), bounds.getBottom(), false));
        g.fillRoundedRectangle (bounds, radius);

        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip; clip.addRoundedRectangle (bounds, radius);
            g.reduceClipRegion (clip);

            // soft light from above
            juce::ColourGradient glow (juce::Colours::white.withAlpha (0.035f), bounds.getCentreX(), bounds.getY() - bounds.getHeight() * 0.2f,
                                       juce::Colours::transparentWhite, bounds.getCentreX(), bounds.getY() + bounds.getHeight() * 0.9f, true);
            g.setGradientFill (glow);
            g.fillRect (bounds);

            drawGrid (g);
            if (analyzerMode != 3) drawSpectrum (g);
            if (matchOpen && match != nullptr) drawMatch (g);
            if (grabAmt > 0.01f) drawGrab (g);
            drawBands (g);
            drawNodes (g);
            drawSketch (g);
            drawInfo (g);

            if (dragMode == Drag::lasso && ! lasso.isEmpty())
            {
                g.setColour (col::accent.withAlpha (0.10f));
                g.fillRoundedRectangle (lasso, 3.0f);
                g.setColour (col::accent.withAlpha (0.6f));
                g.drawRoundedRectangle (lasso, 3.0f, 1.0f);
            }
        }

        g.setColour (col::stroke);
        g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
    }

    void EQGraph::drawGrid (juce::Graphics& g)
    {
        // vertical lines
        const double decades[] { 10, 100, 1000, 10000 };
        for (double d : decades)
            for (int m = 1; m <= 9; ++m)
            {
                const double f = d * m;
                if (f < fMin || f > fMax) continue;
                const float x = xForFreq (f);
                g.setColour (juce::Colours::white.withAlpha (m == 1 ? 0.075f : 0.032f));
                g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getY(), 1.0f, plot.getHeight()));
            }

        // frequency labels
        g.setFont (font (10.5f, Weight::medium));
        const std::pair<double, const char*> fl[] { { 20, "20" }, { 50, "50" }, { 100, "100" }, { 200, "200" }, { 500, "500" },
                                                    { 1000, "1k" }, { 2000, "2k" }, { 5000, "5k" }, { 10000, "10k" }, { 20000, "20k" } };
        for (auto& [f, t] : fl)
        {
            const float x = xForFreq (f);
            g.setColour (col::label3);
            g.drawText (t, juce::Rectangle<float> (x - 24, plot.getBottom() + 3, 48, 16), juce::Justification::centred);
        }

        // dB lines
        const double step = rangeDb <= 3 ? 1.0 : rangeDb <= 6 ? 2.0 : rangeDb <= 12 ? 3.0 : 10.0;
        for (double d = -rangeDb; d <= rangeDb + 1e-6; d += step)
        {
            const float y = yForDb (d);
            const bool zero = std::abs (d) < 1e-6;
            g.setColour (juce::Colours::white.withAlpha (zero ? 0.11f : 0.035f));
            g.fillRect (juce::Rectangle<float> (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f));

            if (std::abs (d) > 1e-6 || true)
            {
                g.setColour (zero ? col::label2 : col::label3);
                g.setFont (font (10.0f, Weight::medium));
                const juce::String txt = (d > 0 ? "+" : "") + juce::String ((int) std::round (d));
                g.drawText (txt, juce::Rectangle<float> (plot.getRight() - 40, y - 8, 32, 16), juce::Justification::centredRight);
            }
        }
    }

    void EQGraph::drawSpectrum (juce::Graphics& g)
    {
        if (grabAmt > 0.98f) return;
        const bool layer = grabAmt > 0.01f;
        if (layer) g.beginTransparencyLayer (1.0f - grabAmt);
        struct EndLayer { juce::Graphics& g; bool on; ~EndLayer() { if (on) g.endTransparencyLayer(); } } endLayer { g, layer };
        auto pathFor = [this] (const std::vector<float>& v)
        {
            juce::Path p;
            for (int i = 0; i < numPts; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * i / (numPts - 1);
                const float y = juce::jlimit (plot.getY(), plot.getBottom(), yForSpectrum (v[(size_t) i]));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            return p;
        };

        auto fillUnder = [this] (juce::Path p)
        {
            p.lineTo (plot.getRight(), plot.getBottom());
            p.lineTo (plot.getX(), plot.getBottom());
            p.closeSubPath();
            return p;
        };

        if (analyzerMode == 0 || analyzerMode == 2)
        {
            auto p = pathFor (preDb);
            g.setColour (juce::Colour (0xff5e6a85).withAlpha (analyzerMode == 2 ? 0.16f : 0.075f));
            g.fillPath (fillUnder (p));
        }

        if (analyzerMode == 0 || analyzerMode == 1)
        {
            auto p = pathFor (postDb);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc8d2ff).withAlpha (0.085f), 0, plot.getY() + plot.getHeight() * 0.3f,
                                                     juce::Colour (0xffc8d2ff).withAlpha (0.0f), 0, plot.getBottom(), false));
            g.fillPath (fillUnder (p));
            g.setColour (juce::Colours::white.withAlpha (0.16f));
            g.strokePath (p, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
        }

        if (peakHold)
        {
            auto p = pathFor (holdDb);
            g.setColour (juce::Colour (0xffffd60a).withAlpha (0.45f));
            g.strokePath (p, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
        }
    }

    void EQGraph::drawBands (juce::Graphics& g)
    {
        if (numPts <= 0) return;
        const float y0 = yForDb (0.0);

        // individual bands, selected one on top
        auto drawBand = [&] (int b)
        {
            const auto& p = proc.band[(size_t) b];
            const bool on = p.on->load() > 0.5f;
            const auto c = on ? col::band (b) : col::label3;
            const float a = appear[(size_t) b];
            const float s = selAmt[(size_t) b];

            auto path = curvePath (bandDb[(size_t) b]);
            juce::Path fill (path);
            fill.lineTo (plot.getRight(), y0);
            fill.lineTo (plot.getX(), y0);
            fill.closeSubPath();

            const bool gainType = typeHasGain ((FilterType) (int) p.type->load());
            const float fa = (0.10f + 0.14f * s + 0.06f * hoverAmt[(size_t) b]) * a * (gainType ? 1.0f : 0.55f);
            if (gainType)
            {
                g.setColour (c.withAlpha (fa));
            }
            else
            {
                // cuts: fade the fill out away from the 0 dB line so it doesn't flood the display
                g.setGradientFill (juce::ColourGradient (c.withAlpha (fa), 0, y0, c.withAlpha (0.0f), 0, y0 + plot.getHeight() * 0.45f, false));
            }
            g.fillPath (fill);
            g.setColour (c.withAlpha ((0.35f + 0.45f * s) * a));
            g.strokePath (path, juce::PathStrokeType (1.0f + 0.4f * s));

            // dynamic range indicator
            if (on && p.dyn->load() > 0.5f && typeHasGain ((FilterType) (int) p.type->load()))
            {
                const auto np = nodePos (b);
                const float yStatic = yForDb (juce::jlimit (-rangeDb, rangeDb, (double) p.gain->load()));
                const float yRange = yForDb (juce::jlimit (-rangeDb, rangeDb, (double) (p.gain->load() + p.range->load())));
                const auto top = juce::jmin (yStatic, yRange), bot = juce::jmax (yStatic, yRange);
                g.setColour (c.withAlpha (0.20f + 0.2f * s));
                g.fillRoundedRectangle (juce::Rectangle<float> (np.x - 3.0f, top, 6.0f, juce::jmax (1.0f, bot - top)), 3.0f);
                g.setColour (c.withAlpha (0.8f));
                g.fillRoundedRectangle (juce::Rectangle<float> (np.x - 6.0f, yRange - 1.0f, 12.0f, 2.0f), 1.0f);

                if (proc.isDynamicActive (b))
                {
                    const float yNow = yForDb (juce::jlimit (-rangeDb, rangeDb, (double) proc.getDynamicGain (b)));
                    g.setColour (juce::Colours::white.withAlpha (0.9f));
                    g.drawEllipse (juce::Rectangle<float> (9, 9).withCentre ({ np.x, yNow }), 1.4f);
                }
            }
        };

        for (int b = 0; b < numBands; ++b)
            if (bandShown[(size_t) b] && ! selection.contains (b)) drawBand (b);
        for (int b : selection)
            if (b >= 0 && bandShown[(size_t) b]) drawBand (b);

        // total curve with glow
        bool any = false;
        for (int b = 0; b < numBands; ++b) any |= bandShown[(size_t) b];
        auto tp = curvePath (total);
        if (any)
        {
            g.setColour (juce::Colours::white.withAlpha (0.035f));
            g.strokePath (tp, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.strokePath (tp, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (juce::Colours::white.withAlpha (any ? 0.95f : 0.25f));
        g.strokePath (tp, juce::PathStrokeType (any ? 2.0f : 1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // ghost node on the curve
        if (ghostX >= 0.0f && dragMode == Drag::none)
        {
            const int i = juce::jlimit (0, numPts - 1, juce::roundToInt ((ghostX - plot.getX()) / plot.getWidth() * (numPts - 1)));
            const juce::Point<float> c (ghostX, yForDb (total[(size_t) i]));
            g.setColour (juce::Colours::white.withAlpha (0.15f));
            g.fillEllipse (juce::Rectangle<float> (18, 18).withCentre (c));
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawEllipse (juce::Rectangle<float> (12, 12).withCentre (c), 1.4f);
            g.drawLine (c.x - 3, c.y, c.x + 3, c.y, 1.3f);
            g.drawLine (c.x, c.y - 3, c.x, c.y + 3, 1.3f);
        }
    }

    void EQGraph::drawNodes (juce::Graphics& g)
    {
        auto drawNode = [&] (int b)
        {
            const auto& p = proc.band[(size_t) b];
            const bool on = p.on->load() > 0.5f;
            const auto c = col::band (b);
            const auto pos = nodePos (b);
            const float h = hoverAmt[(size_t) b], s = selAmt[(size_t) b], a = appear[(size_t) b];
            const float r = (nodeRadius + 1.5f * h + 0.5f * s) * (0.4f + 0.6f * a);

            if (on)
            {
                // halo
                for (int k = 3; k >= 1; --k)
                {
                    g.setColour (c.withAlpha ((0.06f + 0.06f * s + 0.04f * h) * a));
                    g.fillEllipse (juce::Rectangle<float> (2 * (r + k * 3.0f), 2 * (r + k * 3.0f)).withCentre (pos));
                }
            }

            juce::Path dot; dot.addEllipse (juce::Rectangle<float> (2 * r, 2 * r).withCentre (pos));
            juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 6, { 0, 2 }).drawForPath (g, dot);

            if (on)
            {
                g.setGradientFill (juce::ColourGradient (c.brighter (0.25f), pos.x, pos.y - r, c.darker (0.15f), pos.x, pos.y + r, false));
                g.fillPath (dot);
            }
            else
            {
                g.setColour (juce::Colour (0xff2c2c2e));
                g.fillPath (dot);
                g.setColour (c.withAlpha (0.6f));
                g.strokePath (dot, juce::PathStrokeType (1.4f));
            }

            g.setColour (juce::Colours::white.withAlpha (0.25f + 0.7f * s));
            g.strokePath (dot, juce::PathStrokeType (1.0f + 0.6f * s));

            // band number
            g.setColour (on ? juce::Colours::white.withAlpha (0.95f) : col::label2);
            g.setFont (font (9.0f, Weight::semibold));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (2 * r, 2 * r).withCentre (pos).translated (0, 0.5f),
                        juce::Justification::centred, false);
        };

        for (int b = 0; b < numBands; ++b)
            if (isUsed (b) && ! selection.contains (b)) drawNode (b);
        for (int b : selection)
            if (b >= 0 && isUsed (b)) drawNode (b);
    }

    void EQGraph::drawInfo (juce::Graphics& g)
    {
        // empty state
        bool any = false;
        for (int b = 0; b < numBands; ++b) any |= isUsed (b);
        if (! any && ! grabActive && ! matchOpen && ! sketchMode)
        {
            const auto c = juce::Point<float> (plot.getCentreX(), yForDb (0.0) - 34.0f);
            const juce::String t = "Double-click anywhere or click the curve to add a band";
            g.setFont (font (12.5f, Weight::medium));
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t) + 28.0f;
            auto pill = juce::Rectangle<float> (w, 30).withCentre (c);
            g.setColour (juce::Colour (0xcc1c1c1f));
            g.fillRoundedRectangle (pill, 15.0f);
            g.setColour (col::stroke);
            g.drawRoundedRectangle (pill.reduced (0.5f), 15.0f, 1.0f);
            g.setColour (col::label2);
            g.drawText (t, pill, juce::Justification::centred);
        }

        // node info bubble
        const int b = dragMode == Drag::node ? primary : hoverNode;
        if (b >= 0 && isUsed (b))
        {
            const auto& p = proc.band[(size_t) b];
            const auto t = (FilterType) (int) p.type->load();
            const float f = p.freq->load();

            juce::String l1 = typeNames()[(int) t];
            if (p.placement() != Placement::stereo) l1 << "  " << placementNames()[(int) p.placement()];
            if (p.dyn->load() > 0.5f && typeHasGain (t)) l1 << "  Dynamic";

            juce::String l2 = formatFreq (f);
            if (typeHasGain (t)) l2 << "   " << formatDb (p.gain->load());
            if (typeHasSlope (t)) l2 << "   " << slopeChoices[(size_t) juce::jlimit (0, 8, (int) p.slope->load())] << " dB/oct";
            l2 << "   Q " << juce::String (p.q->load(), 2);
            const juce::String l3 = noteName (f);

            const auto f1 = font (11.0f, Weight::semibold), f2 = font (12.5f, Weight::semibold), f3 = font (10.5f, Weight::medium);
            const float w = juce::jmax (juce::GlyphArrangement::getStringWidth (f2, l2), juce::GlyphArrangement::getStringWidth (f1, l1) + 60.0f) + 24.0f;
            const auto np = nodePos (b);
            auto r = juce::Rectangle<float> (w, 50.0f).withCentre ({ np.x, np.y - 46.0f });
            if (r.getY() < plot.getY() + 4) r.setY (np.y + 20.0f);
            r = r.constrainedWithin (plot.reduced (6));

            juce::Path bp; bp.addRoundedRectangle (r, 10.0f);
            juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 16, { 0, 6 }).drawForPath (g, bp);
            g.setColour (juce::Colour (0xf2232326));
            g.fillPath (bp);
            g.setColour (col::strokeHi);
            g.strokePath (bp, juce::PathStrokeType (1.0f));

            auto inner = r.reduced (12, 7);
            auto row1 = inner.removeFromTop (16);
            g.setColour (col::band (b));
            g.fillEllipse (juce::Rectangle<float> (7, 7).withCentre ({ row1.getX() + 3.5f, row1.getCentreY() }));
            g.setFont (f1);
            g.setColour (col::label2);
            g.drawText (l1, row1.withTrimmedLeft (13), juce::Justification::centredLeft);
            g.setFont (f3);
            g.setColour (col::label3);
            g.drawText (l3, row1, juce::Justification::centredRight);
            g.setFont (f2);
            g.setColour (col::label);
            g.drawText (l2, inner, juce::Justification::centredLeft);
        }
        else if (mouseInside && dragMode == Drag::none && plot.contains (mousePos) && ! grabActive)
        {
            // cursor read-out
            const double f = freqForX (mousePos.x);
            const juce::String t = formatFreq ((float) f) + "   " + noteName (f) + "   " + formatDb ((float) dbForY (mousePos.y));
            g.setFont (font (10.5f, Weight::medium));
            g.setColour (col::label3);
            g.drawText (t, juce::Rectangle<float> (plot.getX() + 14, plot.getBottom() - 26, 300, 18), juce::Justification::centredLeft);
        }
    }

    //==============================================================================
    void EQGraph::mouseMove (const juce::MouseEvent& e)
    {
        mousePos = e.position;
        mouseInside = true;
        hoverNode = nodeAt (e.position);
        ghostX = -1.0f;

        if (grabActive)
        {
            if (hoverNode >= 0) endGrab();
            else
            {
                hoverPeak = peakAt (e.position);
                setMouseCursor (hoverPeak >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
                repaint();
                return;
            }
        }

        if (e.position.getDistanceFrom (armPos) > 10.0f)
        {
            armPos = e.position;
            armTime = juce::Time::getMillisecondCounterHiRes();
        }

        if (sketchMode)
        {
            setMouseCursor (hoverNode >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);
            repaint();
            return;
        }

        if (hoverNode < 0 && plot.contains (e.position) && numPts > 1)
        {
            const int i = juce::jlimit (0, numPts - 1, juce::roundToInt ((e.position.x - plot.getX()) / plot.getWidth() * (numPts - 1)));
            if (std::abs (yForDb (total[(size_t) i]) - e.position.y) < 9.0f)
                ghostX = e.position.x;
        }
        setMouseCursor (hoverNode >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    void EQGraph::mouseExit (const juce::MouseEvent&)
    {
        mouseInside = false;
        armTime = 0.0;
        endGrab();
        hoverNode = -1;
        ghostX = -1.0f;
        repaint();
    }

    void EQGraph::mouseDown (const juce::MouseEvent& e)
    {
        grabKeyboardFocus();
        dragMoved = false;
        shiftToggleCandidate = false;
        mousePos = e.position;

        int n = nodeAt (e.position);

        if (grabActive)
        {
            const int pk = peakAt (e.position);
            if (pk >= 0 && n < 0 && ! e.mods.isPopupMenu())
            {
                const auto& peak = peaks[(size_t) pk];
                n = createBand (FilterType::bell, peak.f, 0.0, (float) juce::jlimit (0.7, 16.0, peak.q));
            }
            endGrab();
        }

        if (n < 0 && sketchMode && ! e.mods.isPopupMenu() && plot.contains (e.position))
        {
            sketchPts.clear();
            sketchPts.push_back (e.position);
            dragMode = Drag::sketch;
            return;
        }

        if (n < 0 && ghostX >= 0.0f && ! e.mods.isPopupMenu())
        {
            n = createBand (FilterType::bell, freqForX (e.position.x), 0.0);
            ghostX = -1.0f;
        }

        if (n >= 0)
        {
            if (e.mods.isPopupMenu())
            {
                if (! selection.contains (n)) selectOnly (n);
                primary = n;
                showNodeMenu (n);
                return;
            }

            if (e.mods.isAltDown())
            {
                proc.removeBand (n);
                selection.removeFirstMatchingValue (n);
                if (primary == n) primary = selection.isEmpty() ? -1 : selection.getLast();
                hoverNode = -1;
                selectionChanged();
                return;
            }

            if (e.mods.isShiftDown())
            {
                if (selection.contains (n)) shiftToggleCandidate = true;
                else selection.add (n);
            }
            else if (! selection.contains (n))
            {
                selection.clearQuick();
                selection.add (n);
            }
            primary = n;
            selectionChanged();

            proc.undo.beginNewTransaction ("Edit band");
            dragStarts.clear();
            for (int b : selection)
            {
                const auto& p = proc.band[(size_t) b];
                dragStarts.push_back ({ b, (double) p.freq->load(), (double) p.gain->load(), (double) p.q->load() });
                beginGesture (b);
            }
            dragMode = Drag::node;
            return;
        }

        if (e.mods.isPopupMenu())
        {
            showAddMenu (e.position);
            return;
        }

        dragMode = Drag::lasso;
        lasso = {};
        lassoBase = e.mods.isShiftDown() ? selection : juce::Array<int>();
        if (! e.mods.isShiftDown()) clearSelection();
    }

    void EQGraph::mouseDrag (const juce::MouseEvent& e)
    {
        mousePos = e.position;

        if (dragMode == Drag::sketch)
        {
            const auto p = e.position.withX (juce::jlimit (plot.getX(), plot.getRight(), e.position.x))
                                     .withY (juce::jlimit (plot.getY(), plot.getBottom(), e.position.y));
            if (sketchPts.empty() || std::abs (sketchPts.back().x - p.x) >= 1.0f || std::abs (sketchPts.back().y - p.y) >= 2.0f)
                sketchPts.push_back (p);
            repaint();
            return;
        }

        if (dragMode == Drag::node)
        {
            if (e.getDistanceFromDragStart() > 2) dragMoved = true;
            if (! dragMoved) return;

            const double fine = e.mods.isShiftDown() ? 0.12 : 1.0;
            const double octaves = std::log2 (fMax / fMin);
            const double dOct = (double) e.getDistanceFromDragStartX() / plot.getWidth() * octaves * fine;
            const double half = (plot.getHeight() - bottomReserve) * 0.5 * 0.82;
            const double dDb = -(double) e.getDistanceFromDragStartY() / half * rangeDb * fine;
            const bool qOnly = e.mods.isCommandDown();

            for (const auto& s : dragStarts)
            {
                const auto& p = proc.band[(size_t) s.band];
                const auto t = (FilterType) (int) p.type->load();

                if (! qOnly)
                    setBandParam (s.band, "freq", (float) juce::jlimit (fMin, fMax, s.freq * std::pow (2.0, dOct)));

                if (typeHasGain (t) && ! qOnly)
                    setBandParam (s.band, "gain", (float) juce::jlimit (-30.0, 30.0, s.gain + dDb));
                else
                {
                    const double base = typeHasGain (t) ? 1.0 : qBaseFor (t);
                    const double startDisp = cutDisplayDb (s.q, base);
                    const double scale = typeHasGain (t) ? 12.0 / rangeDb : 1.0;
                    setBandParam (s.band, "q", (float) juce::jlimit (0.025, 40.0, cutQFromDisplay (startDisp + dDb * scale, base)));
                }
            }
            repaint();
            return;
        }

        if (dragMode == Drag::lasso)
        {
            lasso = juce::Rectangle<float> (e.mouseDownPosition, e.position);
            selection = lassoBase;
            for (int b = 0; b < numBands; ++b)
                if (isUsed (b) && lasso.contains (nodePos (b)) && ! selection.contains (b))
                    selection.add (b);
            primary = selection.isEmpty() ? -1 : selection.getLast();
            selectionChanged();
        }
    }

    void EQGraph::mouseUp (const juce::MouseEvent&)
    {
        if (dragMode == Drag::sketch)
        {
            dragMode = Drag::none;
            finishSketch();
            return;
        }

        if (dragMode == Drag::node)
        {
            for (const auto& s : dragStarts) endGesture (s.band);
            dragStarts.clear();

            if (shiftToggleCandidate && ! dragMoved)
            {
                selection.removeFirstMatchingValue (primary);
                primary = selection.isEmpty() ? -1 : selection.getLast();
                selectionChanged();
            }
        }
        dragMode = Drag::none;
        lasso = {};
        repaint();
    }

    void EQGraph::mouseDoubleClick (const juce::MouseEvent& e)
    {
        if (sketchMode) return;
        const int n = nodeAt (e.position);
        if (n >= 0)
        {
            proc.undo.beginNewTransaction ("Toggle band");
            const bool on = proc.band[(size_t) n].on->load() > 0.5f;
            proc.setParam (bandId (n, "on"), on ? 0.0f : 1.0f);
            return;
        }

        if (! plot.contains (e.position)) return;
        const double f = freqForX (e.position.x);
        const FilterType t = f < 22.0 ? FilterType::lowCut : f > 19000.0 ? FilterType::highCut : FilterType::bell;
        createBand (t, f, dbForY (e.position.y));
    }

    void EQGraph::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
    {
        int b = nodeAt (e.position);
        if (b < 0) b = primary;
        if (b < 0 || ! isUsed (b)) return;

        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastWheelTime > 600) proc.undo.beginNewTransaction ("Change Q");
        lastWheelTime = now;

        const float delta = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
        const double fine = e.mods.isShiftDown() ? 0.25 : 1.0;
        const auto& p = proc.band[(size_t) b];
        auto* prm = proc.param (bandId (b, "q"));
        prm->beginChangeGesture();
        setBandParam (b, "q", (float) juce::jlimit (0.025, 40.0, (double) p.q->load() * std::pow (2.0, delta * 2.5 * fine)));
        prm->endChangeGesture();
        hoverNode = b;
    }

    bool EQGraph::keyPressed (const juce::KeyPress& k)
    {
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        {
            if (selection.isEmpty()) return false;
            auto sel = selection;
            for (int b : sel) proc.removeBand (b);
            clearSelection();
            return true;
        }
        if (k == juce::KeyPress::escapeKey)
        {
            if (grabActive) endGrab();
            else if (sketchMode) setSketchMode (false);
            else clearSelection();
            return true;
        }
        if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'Z' || k.getKeyCode() == 'z'))
        {
            if (k.getModifiers().isShiftDown()) proc.undo.redo(); else proc.undo.undo();
            return true;
        }
        if (k.getModifiers().isCommandDown() && (k.getKeyCode() == 'A' || k.getKeyCode() == 'a'))
        {
            selection.clearQuick();
            for (int b = 0; b < numBands; ++b) if (isUsed (b)) selection.add (b);
            primary = selection.isEmpty() ? -1 : selection.getFirst();
            selectionChanged();
            return true;
        }
        return false;
    }

    //==============================================================================
    void EQGraph::showNodeMenu (int b)
    {
        const auto& p = proc.band[(size_t) b];
        const int type = (int) p.type->load();

        juce::PopupMenu shapes;
        for (int i = 0; i < typeNames().size(); ++i) shapes.addItem (100 + i, typeNames()[i], true, i == type);

        juce::PopupMenu slopes;
        for (int i = 0; i < (int) slopeChoices.size(); ++i)
            slopes.addItem (200 + i, juce::String (slopeChoices[(size_t) i]) + " dB/oct", true, i == (int) p.slope->load());

        juce::PopupMenu place;
        for (int i = 0; i < placementNames().size(); ++i) place.addItem (300 + i, placementNames()[i], true, i == (int) p.placement());

        juce::PopupMenu m;
        m.addSectionHeader ("Band " + juce::String (b + 1));
        m.addSubMenu ("Shape", shapes);
        if (typeHasSlope ((FilterType) type)) m.addSubMenu ("Slope", slopes);
        m.addSubMenu ("Stereo Placement", place);
        m.addItem (1, "Dynamic", typeHasGain ((FilterType) type), p.dyn->load() > 0.5f);
        m.addSeparator();
        m.addItem (2, "Enabled", true, p.on->load() > 0.5f);
        m.addItem (3, "Listen (solo)", true, proc.getSoloBand() == b);
        m.addItem (5, "Reset Gain", typeHasGain ((FilterType) type));
        m.addSeparator();
        m.addItem (4, "Delete Band");

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                         [this, b] (int r)
                         {
                             if (r == 0) return;
                             proc.undo.beginNewTransaction ("Edit band");
                             const auto& p = proc.band[(size_t) b];
                             if (r >= 100 && r < 200) proc.setParam (bandId (b, "type"), (float) (r - 100));
                             else if (r >= 200 && r < 300) proc.setParam (bandId (b, "slope"), (float) (r - 200));
                             else if (r >= 300 && r < 400) proc.setParam (bandId (b, "stereo"), (float) (r - 300));
                             else if (r == 1) proc.setParam (bandId (b, "dyn"), p.dyn->load() > 0.5f ? 0.0f : 1.0f);
                             else if (r == 2) proc.setParam (bandId (b, "on"), p.on->load() > 0.5f ? 0.0f : 1.0f);
                             else if (r == 3) proc.setSoloBand (proc.getSoloBand() == b ? -1 : b);
                             else if (r == 5) proc.setParam (bandId (b, "gain"), 0.0f);
                             else if (r == 4)
                             {
                                 proc.removeBand (b);
                                 selection.removeFirstMatchingValue (b);
                                 if (primary == b) primary = selection.isEmpty() ? -1 : selection.getLast();
                                 selectionChanged();
                             }
                         });
    }

    void EQGraph::showAddMenu (juce::Point<float> at)
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Add Band at " + formatFreq ((float) freqForX (at.x)));
        for (int i = 0; i < typeNames().size(); ++i) m.addItem (100 + i, typeNames()[i]);
        const double f = freqForX (at.x), gdb = dbForY (at.y);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(),
                         [this, f, gdb] (int r)
                         {
                             if (r >= 100) createBand ((FilterType) (r - 100), f, gdb);
                         });
    }

    void EQGraph::showAnalyzerMenu()
    {
        juce::PopupMenu show, res, speed, tilt;
        const char* names[] { "Pre + Post", "Post", "Pre", "Off" };
        for (int i = 0; i < 4; ++i) show.addItem (1 + i, names[i], true, analyzerMode == i);
        const char* resNames[] { "Low", "Medium", "High", "Maximum" };
        for (int i = 0; i < 4; ++i) res.addItem (10 + i, resNames[i], true, analyzerRes == i);
        const char* speedNames[] { "Slow", "Medium", "Fast" };
        for (int i = 0; i < 3; ++i) speed.addItem (20 + i, speedNames[i], true, analyzerSpeed == i);
        const double tilts[] { 0.0, 3.0, 4.5, 6.0 };
        for (int i = 0; i < 4; ++i) tilt.addItem (30 + i, juce::String (tilts[i], 1) + " dB/oct", true, std::abs (tiltDb - tilts[i]) < 0.01);

        juce::PopupMenu m;
        m.addSectionHeader ("Spectrum Analyzer");
        m.addSubMenu ("Show", show);
        m.addSubMenu ("Resolution", res);
        m.addSubMenu ("Speed", speed);
        m.addSubMenu ("Tilt", tilt);
        m.addSeparator();
        m.addItem (40, "Peak Hold", true, peakHold);
        m.addItem (41, "Reset Peak Hold", peakHold);
        m.addItem (42, "Spectrum Grab (hover to freeze)", true, grabEnabled);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&analyzerButton),
                         [this, tilts] (int r)
                         {
                             if (r <= 0) return;
                             if (r >= 1 && r <= 4)
                             {
                                 analyzerMode = r - 1;
                                 const char* labels[] { "Analyzer: Pre+Post", "Analyzer: Post", "Analyzer: Pre", "Analyzer: Off" };
                                 analyzerButton.setText (labels[analyzerMode]);
                                 for (auto& v : preDb) v = -120.0f;
                                 for (auto& v : postDb) v = -120.0f;
                             }
                             else if (r >= 10 && r < 14) setAnalyzerResolution (r - 10);
                             else if (r >= 20 && r < 23) analyzerSpeed = r - 20;
                             else if (r >= 30 && r < 34) tiltDb = tilts[r - 30];
                             else if (r == 40) { peakHold = ! peakHold; for (auto& v : holdDb) v = -120.0f; }
                             else if (r == 41) { for (auto& v : holdDb) v = -120.0f; }
                             else if (r == 42) grabEnabled = ! grabEnabled;
                             saveAnalyzerPrefs();
                         });
    }

    //==============================================================================
    // Spectrum Grab
    void EQGraph::activateGrabAt (juce::Point<float> p)
    {
        mousePos = armPos = p;
        mouseInside = true;
        computeGrab();
        grabActive = true;
        grabAmt = 1.0f;
        hoverPeak = peakAt (p);
    }

    void EQGraph::endGrab()
    {
        if (! grabActive) return;
        grabActive = false;
        hoverPeak = -1;
        armTime = juce::Time::getMillisecondCounterHiRes() + 400.0; // don't re-arm instantly
        repaint();
    }

    void EQGraph::computeGrab()
    {
        // detailed spectrum: 16k FFT, three overlapping frames from the last 32k samples
        constexpr int order = 14, n = 1 << order;
        juce::dsp::FFT f (order);
        juce::dsp::WindowingFunction<float> win ((size_t) n, juce::dsp::WindowingFunction<float>::hann, false);
        std::vector<float> ring ((size_t) ScopeRing::size), buf ((size_t) n * 2);
        const auto& src = analyzerMode == 2 ? proc.preScope : proc.postScope;
        src.readLatest (ring.data(), ScopeRing::size);

        std::vector<double> power ((size_t) n / 2 + 1, 0.0);
        for (int frame = 0; frame < 3; ++frame)
        {
            const int start = ScopeRing::size - n - frame * (n / 2);
            std::copy (ring.begin() + start, ring.begin() + start + n, buf.begin());
            std::fill (buf.begin() + n, buf.end(), 0.0f);
            win.multiplyWithWindowingTable (buf.data(), (size_t) n);
            f.performFrequencyOnlyForwardTransform (buf.data(), true);
            for (int k = 0; k <= n / 2; ++k) power[(size_t) k] += (double) buf[(size_t) k] * buf[(size_t) k] / 3.0;
        }

        const double sr = uiSampleRate();
        const double binHz = sr / n;
        const double norm = (2.0 / (n * 0.5)) * (2.0 / (n * 0.5));
        std::vector<double> prefix2 ((size_t) n / 2 + 2, 0.0);
        for (int k = 0; k <= n / 2; ++k) prefix2[(size_t) k + 1] = prefix2[(size_t) k] + power[(size_t) k] * norm;
        auto cum = [&] (double bin)
        {
            bin = juce::jlimit (0.0, (double) (n / 2), bin + 0.5);
            const int i0 = (int) bin;
            return prefix2[(size_t) i0] + (prefix2[(size_t) juce::jmin (i0 + 1, n / 2 + 1)] - prefix2[(size_t) i0]) * (bin - i0);
        };

        grabFreq = makeLogGrid (520, 20.0, juce::jmin (20000.0, 0.45 * sr));
        grabDb.assign (grabFreq.size(), -120.0);
        const double r = std::pow (2.0, 1.0 / 48.0);
        for (size_t i = 0; i < grabFreq.size(); ++i)
        {
            double lo = grabFreq[i] / r / binHz, hi = grabFreq[i] * r / binHz;
            if (hi - lo < 1.0) { const double c = 0.5 * (lo + hi); lo = c - 0.5; hi = c + 0.5; }
            const double pw = juce::jmax (1.0e-14, (cum (hi) - cum (lo)) / (hi - lo));
            grabDb[i] = 10.0 * std::log10 (pw) + tiltDb * std::log2 (grabFreq[i] / 1000.0);
        }

        // peaks = local maxima that stand out from their surroundings (prominence)
        peaks.clear();
        const int N = (int) grabDb.size();
        std::vector<Peak> cand;
        for (int i = 3; i < N - 3; ++i)
        {
            const double v = grabDb[(size_t) i];
            bool isMax = true;
            for (int k = -3; k <= 3 && isMax; ++k) if (k != 0 && grabDb[(size_t) (i + k)] > v) isMax = false;
            if (! isMax || grabFreq[(size_t) i] < 25.0 || grabFreq[(size_t) i] > 18000.0 || v < -90.0) continue;

            double minL = v, minR = v;
            for (int j = i - 1; j >= 0 && grabDb[(size_t) j] <= v; --j) minL = juce::jmin (minL, grabDb[(size_t) j]);
            for (int j = i + 1; j < N && grabDb[(size_t) j] <= v; ++j) minR = juce::jmin (minR, grabDb[(size_t) j]);
            const double prom = v - juce::jmax (minL, minR);
            // must stand out locally and from the broadband level around it (+/- 1 octave)
            if (prom < 6.0) continue;
            double around = 0; int cntA = 0;
            for (int j = juce::jmax (0, i - 48); j < juce::jmin (N, i + 48); ++j) { around += grabDb[(size_t) j]; ++cntA; }
            if (v - around / juce::jmax (1, cntA) < 6.0) continue;

            int lo = i, hi = i;
            while (lo > 0 && grabDb[(size_t) lo] > v - 3.0) --lo;
            while (hi < N - 1 && grabDb[(size_t) hi] > v - 3.0) ++hi;
            const double bw = juce::jmax (1.0, grabFreq[(size_t) hi] - grabFreq[(size_t) lo]);
            cand.push_back ({ grabFreq[(size_t) i], v, juce::jlimit (0.5, 20.0, grabFreq[(size_t) i] / bw), prom });
        }

        // strongest first, keep them at least 1/6 octave apart
        std::vector<std::pair<double, Peak>> scored;
        for (auto& c : cand)
        {
            scored.push_back ({ c.prom, c });
        }
        std::sort (scored.begin(), scored.end(), [] (auto& a, auto& b) { return a.first > b.first; });
        for (auto& [score, pk] : scored)
        {
            juce::ignoreUnused (score);
            bool far = true;
            for (auto& q : peaks) if (std::abs (std::log2 (q.f / pk.f)) < 1.0 / 6.0) { far = false; break; }
            if (far) peaks.push_back (pk);
            if (peaks.size() >= 8) break;
        }
    }

    int EQGraph::peakAt (juce::Point<float> p) const noexcept
    {
        int best = -1;
        float bestD = 34.0f;
        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const juce::Point<float> c (xForFreq (peaks[i].f), yForSpectrum (peaks[i].db));
            const float d = std::abs (c.x - p.x) + 0.25f * std::abs (c.y - p.y);
            if (d < bestD) { bestD = d; best = (int) i; }
        }
        return best;
    }

    void EQGraph::drawGrab (juce::Graphics& g)
    {
        if (grabDb.empty()) return;
        const float a = grabAmt;

        juce::Path p;
        for (size_t i = 0; i < grabFreq.size(); ++i)
        {
            const float x = xForFreq (grabFreq[i]);
            const float y = juce::jlimit (plot.getY(), plot.getBottom(), yForSpectrum (grabDb[i]));
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        juce::Path fillP (p);
        fillP.lineTo (xForFreq (grabFreq.back()), plot.getBottom());
        fillP.lineTo (xForFreq (grabFreq.front()), plot.getBottom());
        fillP.closeSubPath();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffc8d2ff).withAlpha (0.16f * a), 0, plot.getY() + plot.getHeight() * 0.3f,
                                                 juce::Colour (0xffc8d2ff).withAlpha (0.0f), 0, plot.getBottom(), false));
        g.fillPath (fillP);
        g.setColour (juce::Colours::white.withAlpha (0.45f * a));
        g.strokePath (p, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));

        for (size_t i = 0; i < peaks.size(); ++i)
        {
            const juce::Point<float> c (xForFreq (peaks[i].f), yForSpectrum (peaks[i].db));
            const bool hot = (int) i == hoverPeak;
            if (hot)
            {
                g.setColour (col::yellow.withAlpha (0.18f * a));
                g.fillEllipse (juce::Rectangle<float> (26, 26).withCentre (c));
                g.setColour (col::yellow.withAlpha (a));
                g.drawEllipse (juce::Rectangle<float> (13, 13).withCentre (c), 1.6f);
                g.setColour (col::yellow.withAlpha (0.35f * a));
                g.fillRect (juce::Rectangle<float> (c.x - 0.5f, c.y + 8, 1.0f, plot.getBottom() - c.y - 8));
            }
            g.setColour (juce::Colours::white.withAlpha ((hot ? 1.0f : 0.75f) * a));
            g.fillEllipse (juce::Rectangle<float> (hot ? 6.0f : 5.0f, hot ? 6.0f : 5.0f).withCentre (c));
        }

        if (hoverPeak >= 0)
        {
            const auto& pk = peaks[(size_t) hoverPeak];
            const juce::Point<float> c (xForFreq (pk.f), yForSpectrum (pk.db));
            juce::String t = formatFreq ((float) pk.f) + "   " + noteName (pk.f) + "   Q " + juce::String (pk.q, 1);
            drawPill (g, { c.x, juce::jmax (plot.getY() + 60.0f, c.y - 30.0f) }, t, col::yellow);
        }

        drawPill (g, { plot.getCentreX(), plot.getY() + 26.0f },
                  peaks.empty() ? "Spectrum frozen  -  no clear peaks here"
                                : "Spectrum frozen  -  click a peak to add a band, Esc to release", col::yellow.withAlpha (0.0f));
    }

    void EQGraph::drawPill (juce::Graphics& g, juce::Point<float> centre, const juce::String& text, juce::Colour dot) const
    {
        const auto f = font (11.5f, Weight::semibold);
        const bool hasDot = dot.getAlpha() > 0;
        const float w = juce::GlyphArrangement::getStringWidth (f, text) + (hasDot ? 36.0f : 26.0f);
        auto r = juce::Rectangle<float> (w, 26.0f).withCentre (centre).constrainedWithin (plot.reduced (6));
        juce::Path bp; bp.addRoundedRectangle (r, 13.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.45f), 12, { 0, 4 }).drawForPath (g, bp);
        g.setColour (juce::Colour (0xf0232326));
        g.fillPath (bp);
        g.setColour (col::strokeHi);
        g.strokePath (bp, juce::PathStrokeType (1.0f));
        auto t = r.reduced (13, 0);
        if (hasDot)
        {
            g.setColour (dot);
            g.fillEllipse (juce::Rectangle<float> (7, 7).withCentre ({ t.getX() + 3.5f, t.getCentreY() }));
            t.removeFromLeft (12);
        }
        g.setColour (col::label);
        g.setFont (f);
        g.drawText (text, t, juce::Justification::centredLeft, false);
    }

    //==============================================================================
    // EQ Sketch
    void EQGraph::drawSketch (juce::Graphics& g)
    {
        if (sketchMode && sketchPts.empty() && dragMode == Drag::none)
            drawPill (g, { plot.getCentreX(), plot.getY() + 26.0f }, "Sketch: draw the curve you want, release to create bands", col::accent);

        if (sketchPts.size() < 2) return;
        juce::Path p;
        p.startNewSubPath (sketchPts.front());
        for (size_t i = 1; i < sketchPts.size(); ++i) p.lineTo (sketchPts[i]);
        g.setColour (col::accent.withAlpha (0.25f));
        g.strokePath (p, juce::PathStrokeType (8.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (col::accent);
        if (sketchFitting)
        {
            juce::Path dashed;
            const float dashes[] { 6.0f, 5.0f };
            juce::PathStrokeType (2.0f).createDashedStroke (dashed, p, dashes, 2);
            g.fillPath (dashed);
        }
        else
            g.strokePath (p, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void EQGraph::finishSketch()
    {
        if (sketchPts.size() < 3) { sketchPts.clear(); repaint(); return; }

        // the drawn line as (log f, dB), sampled on the fit grid; ends are held flat
        std::vector<std::pair<double, double>> pts;
        for (auto& p : sketchPts)
            pts.push_back ({ std::log2 (freqForX (p.x)), juce::jlimit (-24.0, 24.0, dbForY (p.y)) });

        auto grid = makeLogGrid (240);
        std::vector<double> target (grid.size(), 0.0), weight (grid.size(), 1.0);
        double lfLo = 1e9, lfHi = -1e9;
        for (auto& pt : pts) { lfLo = juce::jmin (lfLo, pt.first); lfHi = juce::jmax (lfHi, pt.first); }

        for (size_t i = 0; i < grid.size(); ++i)
        {
            const double lf = std::log2 (grid[i]);
            if (lf <= lfLo) { target[i] = std::min_element (pts.begin(), pts.end())->second; weight[i] = 0.5; continue; }
            if (lf >= lfHi) { target[i] = std::max_element (pts.begin(), pts.end())->second; weight[i] = 0.5; continue; }
            // last stroke segment covering this frequency wins (lets you redraw over a part)
            bool found = false;
            for (size_t k = pts.size() - 1; k > 0 && ! found; --k)
            {
                const auto a = pts[k - 1], b = pts[k];
                const double lo = juce::jmin (a.first, b.first), hi = juce::jmax (a.first, b.first);
                if (lf >= lo && lf <= hi)
                {
                    const double t = hi > lo ? (lf - a.first) / (b.first - a.first) : 0.0;
                    target[i] = a.second + (b.second - a.second) * t;
                    found = true;
                }
            }
        }

        sketchFitting = true;
        repaint();
        auto aliveFlag = alive;
        fitPool.addJob ([this, aliveFlag, grid, target, weight]
        {
            FitOptions o;
            o.maxBands = 12;
            o.maxGain = 24.0;
            o.minQ = 0.3;
            o.maxQ = 8.0;
            o.tolerance = 0.25;
            auto res = CurveFitter::fit (grid, target, weight, o);
            juce::MessageManager::callAsync ([this, aliveFlag, res]
            {
                if (! aliveFlag->load()) return;
                sketchFitting = false;
                sketchPts.clear();

                proc.undo.beginNewTransaction ("Sketch");
                for (int b = 0; b < numBands; ++b)
                    if (isUsed (b)) { proc.setParam (bandId (b, "used"), 0.0f); proc.resetBand (b); }
                int slot = 0;
                for (const auto& s : res.bands)
                {
                    if (slot >= numBands) break;
                    proc.resetBand (slot);
                    proc.setParam (bandId (slot, "type"), (float) s.type);
                    proc.setParam (bandId (slot, "freq"), (float) s.freq);
                    proc.setParam (bandId (slot, "gain"), (float) s.gainDb);
                    proc.setParam (bandId (slot, "q"), (float) s.q);
                    proc.setParam (bandId (slot, "used"), 1.0f);
                    appear[(size_t) slot] = 0.0f;
                    ++slot;
                }
                proc.currentPresetName = "Sketch";
                clearSelection();
                repaint();
            });
        });
    }

    //==============================================================================
    // Match EQ overlay
    void EQGraph::drawMatch (juce::Graphics& g)
    {
        const auto& f = match->freqs();
        const auto& ref = match->referenceDb();
        const auto& cur = match->currentDb();

        // place both spectra around the analyzer's level, aligned to each other
        double mean = 0; int cnt = 0;
        const auto& base = ! cur.empty() ? cur : ref;
        for (size_t i = 0; i < f.size() && ! base.empty(); ++i) if (f[i] >= 100 && f[i] <= 8000) { mean += base[i]; ++cnt; }
        mean = cnt > 0 ? mean / cnt : 0.0;
        const double display = -36.0;

        auto spec = [&] (const std::vector<double>& v, double shift, juce::Colour c)
        {
            if (v.empty()) return;
            juce::Path p;
            for (size_t i = 0; i < f.size(); ++i)
            {
                const double db = v[i] - mean + display + shift + tiltDb * std::log2 (f[i] / 1000.0);
                const float x = xForFreq (f[i]);
                const float y = juce::jlimit (plot.getY(), plot.getBottom(), yForSpectrum (db));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            g.setColour (c.withAlpha (0.18f));
            g.strokePath (p, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (c.withAlpha (0.9f));
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };

        const double refShift = cur.empty() ? 0.0 : -match->levelOffset();
        spec (ref, refShift, col::band (1));
        spec (cur, 0.0, col::band (0));

        auto eqCurve = [&] (const std::vector<double>& v)
        {
            juce::Path p;
            for (size_t i = 0; i < f.size(); ++i)
            {
                const float x = xForFreq (f[i]);
                const float y = yForDb (juce::jlimit (-rangeDb * 1.2, rangeDb * 1.2, v[i]));
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            return p;
        };

        if (! match->targetDb().empty())
        {
            juce::Path dashed;
            const float dashes[] { 3.0f, 4.0f };
            juce::PathStrokeType (1.2f).createDashedStroke (dashed, eqCurve (match->targetDb()), dashes, 2);
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillPath (dashed);
        }
        if (! match->previewDb().empty())
        {
            auto p = eqCurve (match->previewDb());
            g.setColour (col::green.withAlpha (0.18f));
            g.strokePath (p, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (col::green);
            g.strokePath (p, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // legend
        auto leg = juce::Rectangle<float> (plot.getX() + 16, plot.getY() + 48, 400, 16);
        auto item = [&] (const juce::String& t, juce::Colour c, bool on)
        {
            if (! on) return;
            g.setColour (c);
            g.fillEllipse (juce::Rectangle<float> (7, 7).withCentre ({ leg.getX() + 3.5f, leg.getCentreY() }));
            g.setColour (col::label2);
            g.setFont (font (11.0f, Weight::medium));
            const float w = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), t);
            g.drawText (t, leg.withTrimmedLeft (12), juce::Justification::centredLeft);
            leg.removeFromLeft (w + 30);
        };
        item ("Reference", col::band (1), ! ref.empty());
        item ("Your track", col::band (0), ! cur.empty());
        item ("Difference", juce::Colours::white.withAlpha (0.6f), ! match->targetDb().empty());
        item ("Match curve", col::green, ! match->previewDb().empty());
    }

}
