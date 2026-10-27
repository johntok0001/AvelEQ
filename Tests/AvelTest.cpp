// Offline tests for Avel EQ: DSP behaviour + UI snapshots, no DAW needed.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "DSP/CurveFit.h"
#include <iostream>

using namespace avel;

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::cout << (ok ? "  PASS  " : "  FAIL  ") << what << std::endl;
    if (! ok) ++failures;
}

static void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

struct Rig
{
    AvelProcessor p;
    double sr = 48000.0;
    int block = 512;

    Rig()
    {
        p.setRateAndBufferSizeDetails (sr, block);
        p.prepareToPlay (sr, block);
    }

    void set (const juce::String& id, float v) { p.setParam (id, v); }
    void band (int b, FilterType t, float f, float g, float q, Placement pl = Placement::stereo)
    {
        set (bandId (b, "type"), (float) t); set (bandId (b, "freq"), f); set (bandId (b, "gain"), g);
        set (bandId (b, "q"), q); set (bandId (b, "stereo"), (float) pl); set (bandId (b, "used"), 1.0f);
    }
    void clearAll() { for (int b = 0; b < numBands; ++b) { p.removeBand (b); } }

    // runs a stereo signal through, returns output
    juce::AudioBuffer<float> run (const juce::AudioBuffer<float>& in)
    {
        juce::AudioBuffer<float> out (in);
        juce::MidiBuffer m;
        for (int pos = 0; pos < out.getNumSamples(); pos += block)
        {
            const int n = juce::jmin (block, out.getNumSamples() - pos);
            juce::AudioBuffer<float> v (out.getArrayOfWritePointers(), 2, pos, n);
            p.processBlock (v, m);
        }
        return out;
    }
};

static juce::AudioBuffer<float> sine (double sr, double secs, double hz, float ampL, float ampR)
{
    juce::AudioBuffer<float> b (2, (int) (sr * secs));
    for (int i = 0; i < b.getNumSamples(); ++i)
    {
        const float v = (float) std::sin (2.0 * pi * hz * i / sr);
        b.setSample (0, i, ampL * v);
        b.setSample (1, i, ampR * v);
    }
    return b;
}

static double rmsDb (const juce::AudioBuffer<float>& b, int ch, int from, int to = -1)
{
    if (to < 0) to = b.getNumSamples();
    double s = 0;
    for (int i = from; i < to; ++i) s += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return 10.0 * std::log10 (s / (to - from) + 1e-30) + 3.0103; // sine peak dB
}

static void snapshot (AvelProcessor& p, const juce::String& file, std::function<void (AvelEditor&)> setup, int frames = 40)
{
    std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
    auto* e = dynamic_cast<AvelEditor*> (ed.get());
    ed->setSize (1180, 720);
    if (setup) setup (*e);

    // feed some pink-ish noise so the analyzer shows something
    juce::Random rnd (1);
    juce::AudioBuffer<float> buf (2, 1024);
    juce::MidiBuffer m;
    float b0 = 0, b1 = 0, b2 = 0;
    for (int f = 0; f < frames; ++f)
    {
        for (int i = 0; i < 1024; ++i)
        {
            const float w = rnd.nextFloat() * 2 - 1;
            b0 = 0.99765f * b0 + w * 0.0990460f; b1 = 0.96300f * b1 + w * 0.2965164f; b2 = 0.57000f * b2 + w * 1.0526913f;
            const float v = 0.06f * (b0 + b1 + b2 + w * 0.1848f);
            buf.setSample (0, i, v); buf.setSample (1, i, v * 0.9f);
        }
        p.processBlock (buf, m);
        pump (17);
    }

    auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
    juce::File out (file);
    out.deleteFile();
    juce::FileOutputStream fos (out);
    juce::PNGImageFormat png;
    png.writeImageToStream (img, fos);
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String shotDir = argc > 1 ? argv[1] : juce::File::getCurrentWorkingDirectory().getFullPathName();

    std::cout << "Zero latency" << std::endl;
    {
        Rig r;
        auto in = sine (r.sr, 1.0, 1000.0, 0.25f, 0.25f);
        auto out = r.run (in);
        check (std::abs (rmsDb (out, 0, 24000) - rmsDb (in, 0, 24000)) < 0.01, "no bands = transparent");
        check (r.p.getLatencySamples() == 0, "zero latency reports 0 samples");

        r.band (0, FilterType::bell, 1000, 6, 1);
        out = r.run (in);
        check (std::abs (rmsDb (out, 0, 24000) - rmsDb (in, 0, 24000) - 6.0) < 0.1, "bell +6 dB at 1 kHz: " + juce::String (rmsDb (out, 0, 24000) - rmsDb (in, 0, 24000), 2));

        auto lo = sine (r.sr, 1.0, 100.0, 0.25f, 0.25f);
        out = r.run (lo);
        check (std::abs (rmsDb (out, 0, 24000) - rmsDb (lo, 0, 24000)) < 0.2, "same bell leaves 100 Hz alone");

        // High shelf near Nyquist: matched design must not cramp
        r.set (bandId (0, "used"), 0);
        r.band (1, FilterType::highShelf, 8000, -12, 0.71f);
        auto hi = sine (r.sr, 1.0, 18000.0, 0.25f, 0.25f);
        out = r.run (hi);
        const double expect = analogBandMagDb ({ FilterType::highShelf, 8000, -12, 0.71, 1 }, 18000, r.sr);
        check (std::abs (rmsDb (out, 0, 24000) - rmsDb (hi, 0, 24000) - expect) < 0.6,
               "high shelf -12 at 18 kHz: " + juce::String (rmsDb (out, 0, 24000) - rmsDb (hi, 0, 24000), 2) + " dB (analog " + juce::String (expect, 2) + ")");

        // 96 dB/oct low cut
        r.set (bandId (1, "used"), 0);
        r.band (2, FilterType::lowCut, 200, 0, 0.7071f);
        r.set (bandId (2, "slope"), 8);
        auto s50 = sine (r.sr, 1.0, 50.0, 0.25f, 0.25f);
        out = r.run (s50);
        out = r.run (s50);
        check (rmsDb (out, 0, 24000) - rmsDb (s50, 0, 24000) < -90.0, "96 dB/oct low cut, 2 octaves below: " + juce::String (rmsDb (out, 0, 24000) - rmsDb (s50, 0, 24000), 1) + " dB");
        r.set (bandId (2, "used"), 0);
    }

    std::cout << "Stereo placement" << std::endl;
    {
        Rig r;
        r.band (0, FilterType::bell, 1000, -30, 1, Placement::side);
        auto mono = sine (r.sr, 1.0, 1000.0, 0.25f, 0.25f);
        auto out = r.run (mono);
        double err = 0;
        for (int i = 0; i < out.getNumSamples(); ++i) err = juce::jmax (err, (double) std::abs (out.getSample (0, i) - mono.getSample (0, i)));
        check (err < 1e-5, "Side band does not touch a mono signal");

        r.set (bandId (0, "stereo"), (float) Placement::mid);
        r.run (mono);
        out = r.run (mono);
        check (rmsDb (out, 0, 24000) - rmsDb (mono, 0, 24000) < -25.0, "Mid band cuts a mono signal");

        r.set (bandId (0, "stereo"), (float) Placement::left);
        r.run (mono);
        out = r.run (mono);
        check (rmsDb (out, 0, 24000) - rmsDb (mono, 0, 24000) < -25.0 && std::abs (rmsDb (out, 1, 24000) - rmsDb (mono, 1, 24000)) < 0.01,
               "Left band only affects the left channel");
    }

    std::cout << "Dynamic EQ" << std::endl;
    {
        Rig r;
        r.band (0, FilterType::bell, 1000, 0, 1);
        r.set (bandId (0, "dyn"), 1); r.set (bandId (0, "range"), -12); r.set (bandId (0, "thr"), -30);
        auto loud = sine (r.sr, 1.0, 1000.0, 0.5f, 0.5f);   // -6 dB, 24 dB above threshold
        auto out = r.run (loud);
        const double redLoud = rmsDb (out, 0, 24000) - rmsDb (loud, 0, 24000);
        check (redLoud < -11.0 && redLoud > -12.5, "loud signal is reduced by the range: " + juce::String (redLoud, 2) + " dB");
        check (std::abs (r.p.getDynamicGain (0) + 12.0f) < 0.6f, "UI sees dynamic gain " + juce::String (r.p.getDynamicGain (0), 2));

        auto quiet = sine (r.sr, 1.0, 1000.0, 0.005f, 0.005f); // -46 dB, below threshold
        r.run (quiet);
        out = r.run (quiet);
        check (std::abs (rmsDb (out, 0, 24000) - rmsDb (quiet, 0, 24000)) < 0.3, "quiet signal is untouched");

        auto other = sine (r.sr, 1.0, 100.0, 0.5f, 0.5f);
        r.run (other);
        out = r.run (other);
        check (std::abs (rmsDb (out, 0, 24000) - rmsDb (other, 0, 24000)) < 0.5, "loud signal outside the band does not trigger it");
    }

    std::cout << "Auto gain" << std::endl;
    {
        Rig r;
        r.band (0, FilterType::highShelf, 2000, 12, 0.71f);
        r.set ("autoGain", 1);
        r.run (sine (r.sr, 0.2, 1000.0, 0.1f, 0.1f));
        check (r.p.getAutoGainDb() < -3.0f, "big boost gets negative compensation: " + juce::String (r.p.getAutoGainDb(), 2) + " dB");
    }

    std::cout << "Linear phase" << std::endl;
    {
        Rig r;
        r.band (0, FilterType::bell, 1000, 6, 1);
        r.set ("phase", 2);
        r.set ("lpRes", 1);
        pump (100);
        r.p.prepareToPlay (r.sr, r.block); // hosts re-prepare after a latency change
        const int lat = r.p.getLatencySamples();
        check (lat == 8192, "latency reported: " + juce::String (lat) + " samples");

        for (int i = 0; i < 100; ++i) { pump (10); r.run (sine (r.sr, 0.02, 1000.0, 0.0f, 0.0f)); }

        auto in = sine (r.sr, 2.0, 1000.0, 0.25f, 0.25f);
        auto out = r.run (in);
        const double g = rmsDb (out, 0, 48000) - rmsDb (in, 0, 48000);
        check (std::abs (g - 6.0) < 0.15, "bell +6 dB at 1 kHz in linear phase: " + juce::String (g, 2));

        // impulse: symmetric around the latency
        r.run (sine (r.sr, 0.5, 1000.0, 0.0f, 0.0f)); // flush the sine tail
        juce::AudioBuffer<float> imp (2, 32768);
        imp.clear();
        imp.setSample (0, 0, 1.0f); imp.setSample (1, 0, 1.0f);
        auto ir = r.run (imp);
        int peak = 0;
        for (int i = 0; i < ir.getNumSamples(); ++i) if (std::abs (ir.getSample (0, i)) > std::abs (ir.getSample (0, peak))) peak = i;
        double asym = 0, energy = 0;
        for (int k = 1; k < 4000; ++k)
        {
            asym += std::abs (ir.getSample (0, peak + k) - ir.getSample (0, peak - k));
            energy += std::abs (ir.getSample (0, peak + k));
        }
        check (peak == lat, "impulse peak at the latency: " + juce::String (peak));
        check (asym < energy * 0.01, "impulse response is symmetric (linear phase)");

        // mid/side in linear phase
        r.set (bandId (0, "stereo"), (float) Placement::side);
        r.set (bandId (0, "gain"), -30);
        for (int i = 0; i < 60; ++i) { pump (10); r.run (sine (r.sr, 0.02, 1000.0, 0.0f, 0.0f)); }
        out = r.run (in);
        check (std::abs (rmsDb (out, 0, 48000) - rmsDb (in, 0, 48000)) < 0.1, "side band leaves mono untouched in linear phase");
        r.set ("phase", 0);
        pump (50);
    }

    std::cout << "Natural phase" << std::endl;
    {
        // Natural phase must sound like the zero-latency (analog) filter, just delayed
        auto noise = [] (double sr, double secs)
        {
            juce::Random rnd (5);
            juce::AudioBuffer<float> b (2, (int) (sr * secs));
            for (int i = 0; i < b.getNumSamples(); ++i) { const float v = (rnd.nextFloat() * 2 - 1) * 0.2f; b.setSample (0, i, v); b.setSample (1, i, v * 0.7f); }
            return b;
        };
        int which = 0;
        auto setup = [&which] (Rig& r)
        {
            if (which == 0 || which == 1) r.band (0, FilterType::bell, 300, 8, 2);
            if (which == 0 || which == 2) r.band (1, FilterType::highShelf, 1500, -5, 0.7f);
            if (which == 0 || which == 3) { r.band (2, FilterType::lowCut, 60, 0, 0.9f); r.set (bandId (2, "slope"), 3); }
            if (which == 4) r.band (0, FilterType::bell, 1000, 0.0f, 1);
        };
        for (which = 0; which >= 0; --which) {
        Rig z; setup (z);
        Rig n; setup (n);
        n.set ("phase", 1); n.set ("lpRes", 2);
        pump (100);
        n.p.prepareToPlay (n.sr, n.block);
        const int D = n.p.getLatencySamples();
        check (D == 1024, "natural phase latency " + juce::String (D) + " samples");
        for (int i = 0; i < 80; ++i) { pump (10); n.run (sine (n.sr, 0.02, 1000, 0, 0)); }

        auto in = noise (z.sr, 3.0);
        z.run (sine (z.sr, 0.5, 1000, 0, 0));
        n.run (sine (n.sr, 1.0, 1000, 0, 0));
        auto oz = z.run (in);
        auto on = n.run (in);
        // compare below ~12 kHz (the zero-latency design deviates slightly from analog near Nyquist)
        double e = 0, sig = 0, lpD = 0, lpS = 0;
        const double c = std::exp (-2.0 * pi * 12000.0 / z.sr);
        for (int i = 48000; i < in.getNumSamples() - D; ++i)
        {
            const double d = on.getSample (0, i + D) - oz.getSample (0, i);
            lpD = (1 - c) * d + c * lpD;
            lpS = (1 - c) * oz.getSample (0, i) + c * lpS;
            e += lpD * lpD; sig += lpS * lpS;
        }
        const double nullDb = 10.0 * std::log10 (e / sig + 1e-30);
        check (nullDb < -30.0, "natural phase nulls against zero latency (same phase), case " + juce::String (which) + ": " + juce::String (nullDb, 1) + " dB");
        }
    }

    std::cout << "Curve fit / Match EQ" << std::endl;
    {
        auto grid = makeLogGrid (240);
        std::vector<BandSettings> truth { { FilterType::bell, 250, -4, 1.2, 1 }, { FilterType::bell, 2500, 3, 0.8, 1 }, { FilterType::highShelf, 9000, 2.5, 0.7, 1 } };
        auto t = CurveFitter::response (grid, truth);
        std::vector<double> w (grid.size(), 1.0);
        FitOptions o; o.maxBands = 8;
        auto res = CurveFitter::fit (grid, t, w, o);
        check (res.rmsError < 0.1, "fit reproduces a 3-band curve: rms " + juce::String (res.rmsError, 3) + " dB with " + juce::String ((int) res.bands.size()) + " bands");

        // end to end: reference = noise through a known EQ, current = the same noise
        Rig r;
        avel::ui::MatchEngine me (r.p);
        juce::Random rnd (9);
        auto makeNoise = [&] (double secs)
        {
            juce::AudioBuffer<float> b (2, (int) (r.sr * secs));
            for (int i = 0; i < b.getNumSamples(); ++i) { const float v = (rnd.nextFloat() * 2 - 1) * 0.2f; b.setSample (0, i, v); b.setSample (1, i, v); }
            return b;
        };
        auto shaped = [&] (juce::AudioBuffer<float> b)
        {
            Rig e; e.band (0, FilterType::bell, 400, 5, 1.0f); e.band (1, FilterType::highShelf, 5000, -4, 0.7f);
            return e.run (b);
        };
        auto feed = [&] (const juce::AudioBuffer<float>& b)
        {
            juce::MidiBuffer m;
            for (int pos = 0; pos + 512 <= b.getNumSamples(); pos += 512)
            {
                juce::AudioBuffer<float> blk (2, 512);
                for (int ch = 0; ch < 2; ++ch) blk.copyFrom (ch, 0, b, ch, pos, 512);
                r.p.processBlock (blk, m);
                if ((pos / 512) % 4 == 0) me.tick();
            }
        };
        me.setLearning (avel::ui::MatchEngine::Learn::reference);
        feed (shaped (makeNoise (20.0)));
        me.setLearning (avel::ui::MatchEngine::Learn::current);
        feed (makeNoise (20.0));
        me.setLearning (avel::ui::MatchEngine::Learn::none);
        for (int i = 0; i < 300 && (me.isFitting() || me.fittedBands().empty()); ++i) pump (10);

        const auto& f = me.freqs();
        double maxErr = 0;
        for (size_t i = 0; i < f.size(); ++i)
        {
            if (f[i] < 60 || f[i] > 12000) continue;
            const double want = analogBandMagDb ({ FilterType::bell, 400, 5, 1.0, 1 }, f[i], 1e9) + analogBandMagDb ({ FilterType::highShelf, 5000, -4, 0.7, 1 }, f[i], 1e9);
            maxErr = juce::jmax (maxErr, std::abs (me.previewDb()[i] + me.levelOffset() * 0 - want));
        }
        check (! me.fittedBands().empty() && maxErr < 1.5, "Match EQ recovers a known EQ from audio: max error " + juce::String (maxErr, 2) + " dB, "
               + juce::String ((int) me.fittedBands().size()) + " bands");
        me.apply();
        int used = 0; for (auto& b : r.p.band) used += b.used->load() > 0.5f;
        check (used == (int) me.fittedBands().size(), "Apply creates the bands");
    }

    std::cout << "State" << std::endl;
    {
        Rig r;
        r.band (3, FilterType::lowShelf, 120, 3.5f, 0.8f, Placement::mid);
        r.set (bandId (3, "dyn"), 1);
        juce::MemoryBlock mb;
        r.p.getStateInformation (mb);

        AvelProcessor q;
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        const auto& b = q.band[3];
        check (b.used->load() > 0.5f && std::abs (b.freq->load() - 120.0f) < 0.01f && std::abs (b.gain->load() - 3.5f) < 0.01f
               && (int) b.stereo->load() == (int) Placement::mid && b.dyn->load() > 0.5f, "band restored from saved state");

        // undo
        r.p.undo.clearUndoHistory();
        const int nb = r.p.addBand (FilterType::bell, 2000, 4, 1);
        pump (200);
        check (nb >= 0 && r.p.band[(size_t) nb].used->load() > 0.5f, "add band");
        r.p.undo.undo();
        pump (200);
        check (r.p.band[(size_t) nb].used->load() < 0.5f, "undo removes it again");

        // presets
        r.p.loadFactoryPreset (1);
        int used = 0;
        for (auto& bp : r.p.band) used += bp.used->load() > 0.5f;
        check (used == 5, "factory preset 'Vocal Presence' has 5 bands: " + juce::String (used));
    }

    std::cout << "Performance" << std::endl;
    {
        auto bench = [] (bool linear, bool dynamic, int slope)
        {
            Rig r;
            for (int b = 0; b < numBands; ++b)
            {
                r.band (b, b % 3 == 0 ? FilterType::lowCut : FilterType::bell, 40.0f * std::pow (1.25f, (float) b), 3.0f, 1.0f, (Placement) (b % 5));
                r.set (bandId (b, "slope"), (float) slope);
                r.set (bandId (b, "dyn"), dynamic ? 1.0f : 0.0f);
            }
            if (linear) { r.set ("phase", 2); pump (100); r.p.prepareToPlay (r.sr, r.block); for (int i = 0; i < 50; ++i) { pump (10); r.run (sine (r.sr, 0.02, 1000, 0, 0)); } }
            auto in = sine (r.sr, 10.0, 1000.0, 0.3f, 0.2f);
            r.run (sine (r.sr, 0.5, 1000.0, 0.3f, 0.2f));
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            r.run (in);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            return 100.0 * ms / 10000.0; // % of one core for real time
        };
        std::cout << "    24 bands static, 96 dB/oct cuts: " << juce::String (bench (false, false, 8), 2) << " % CPU" << std::endl;
        std::cout << "    24 bands dynamic:                " << juce::String (bench (false, true, 1), 2) << " % CPU" << std::endl;
        std::cout << "    linear phase (medium):           " << juce::String (bench (true, false, 1), 2) << " % CPU" << std::endl;
    }

    std::cout << "UI snapshots" << std::endl;
    {
        Rig r;
        snapshot (r.p, shotDir + "/ui_empty.png", nullptr);

        r.p.loadFactoryPreset (1);
        snapshot (r.p, shotDir + "/ui_preset.png", [] (AvelEditor&) {});

        snapshot (r.p, shotDir + "/ui_selected.png", [&] (AvelEditor& e)
        {
            auto* graph = dynamic_cast<avel::ui::EQGraph*> (e.getChildComponent (0));
            if (graph != nullptr) graph->selectOnly (2);
        });

        snapshot (r.p, shotDir + "/ui_dynamic.png", [&] (AvelEditor& e)
        {
            auto* graph = dynamic_cast<avel::ui::EQGraph*> (e.getChildComponent (0));
            if (graph != nullptr) graph->selectOnly (3);
        });

        // spectrum grab: noise + two resonances
        {
            Rig g;
            std::unique_ptr<juce::AudioProcessorEditor> ed (g.p.createEditor());
            auto* e = dynamic_cast<AvelEditor*> (ed.get());
            ed->setSize (1180, 720);
            juce::Random rnd (2);
            juce::MidiBuffer m;
            juce::AudioBuffer<float> buf (2, 1024);
            double ph1 = 0, ph2 = 0;
            for (int f = 0; f < 50; ++f)
            {
                for (int i = 0; i < 1024; ++i)
                {
                    ph1 += 2 * pi * 2000.0 / g.sr; ph2 += 2 * pi * 315.0 / g.sr;
                    const float v = (rnd.nextFloat() * 2 - 1) * 0.02f + 0.1f * (float) std::sin (ph1) + 0.08f * (float) std::sin (ph2);
                    buf.setSample (0, i, v); buf.setSample (1, i, v);
                }
                g.p.processBlock (buf, m);
                pump (17);
            }
            auto& graph = e->getGraph();
            graph.activateGrabAt ({ (float) graph.getWidth() * 0.6f, (float) graph.getHeight() * 0.5f });
            bool f2k = false, f315 = false;
            for (int i = 0; i < graph.getNumPeaks(); ++i)
            {
                f2k  |= std::abs (std::log2 (graph.getPeakFreq (i) / 2000.0)) < 0.05;
                f315 |= std::abs (std::log2 (graph.getPeakFreq (i) / 315.0)) < 0.05;
            }
            check (graph.isGrabActive() && f2k && f315, "Spectrum Grab finds the 315 Hz and 2 kHz peaks (" + juce::String (graph.getNumPeaks()) + " peaks)");
            pump (100);
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            juce::File out (shotDir + "/ui_grab.png"); out.deleteFile();
            juce::FileOutputStream fos (out); juce::PNGImageFormat().writeImageToStream (img, fos);
        }

        // match panel with learned spectra
        snapshot (r.p, shotDir + "/ui_match.png", [&] (AvelEditor& e)
        {
            e.setMatchOpen (true);
            auto& me = e.getMatchEngine();
            me.setLearning (avel::ui::MatchEngine::Learn::reference);
        }, 1);
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (r.p.createEditor());
            auto* e = dynamic_cast<AvelEditor*> (ed.get());
            ed->setSize (1180, 720);
            e->setMatchOpen (true);
            auto& me = e->getMatchEngine();
            juce::Random rnd (4);
            juce::MidiBuffer m;
            auto play = [&] (bool bright)
            {
                float lp = 0;
                for (int f = 0; f < 400; ++f)
                {
                    juce::AudioBuffer<float> buf (2, 512);
                    for (int i = 0; i < 512; ++i)
                    {
                        const float w = rnd.nextFloat() * 2 - 1;
                        lp = 0.92f * lp + 0.08f * w;
                        const float v = bright ? 0.12f * w + 0.3f * lp : 0.6f * lp + 0.02f * w;
                        buf.setSample (0, i, v); buf.setSample (1, i, v);
                    }
                    r.p.processBlock (buf, m);
                    if (f % 4 == 0) me.tick();
                }
            };
            r.p.loadFactoryPreset (0);
            me.setLearning (avel::ui::MatchEngine::Learn::reference); play (true);
            me.setLearning (avel::ui::MatchEngine::Learn::current); play (false);
            me.setLearning (avel::ui::MatchEngine::Learn::none);
            for (int i = 0; i < 100; ++i) { e->repaint(); pump (17); }
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            juce::File out (shotDir + "/ui_match.png"); out.deleteFile();
            juce::FileOutputStream fos (out); juce::PNGImageFormat().writeImageToStream (img, fos);
        }

        r.p.loadFactoryPreset (2);
        r.set ("phase", 2);
        snapshot (r.p, shotDir + "/ui_cut.png", [&] (AvelEditor& e)
        {
            auto* graph = dynamic_cast<avel::ui::EQGraph*> (e.getChildComponent (0));
            if (graph != nullptr) graph->selectOnly (0);
        });
        check (true, "snapshots written to " + shotDir);
    }

    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
