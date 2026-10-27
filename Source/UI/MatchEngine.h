#pragma once

// Match EQ: learns the long-term spectrum of a reference (played through the plugin or
// loaded from a file) and of your own track, then fits EQ bands to the difference.

#include <juce_audio_formats/juce_audio_formats.h>
#include "../PluginProcessor.h"
#include "../DSP/Spectrum.h"
#include "../DSP/CurveFit.h"

namespace avel::ui
{
    class MatchEngine : private juce::Thread
    {
    public:
        explicit MatchEngine (AvelProcessor& p)
            : juce::Thread ("Avel EQ match"), proc (p),
              grid (makeLogGrid (240)), weights (matchWeights (grid))
        {
            ref = std::make_shared<SpectrumAccumulator>();
            cur = std::make_shared<SpectrumAccumulator>();
            startThread (juce::Thread::Priority::low);
        }

        ~MatchEngine() override
        {
            *alive = false;
            cancelFit = true;
            signalThreadShouldExit();
            notify();
            stopThread (3000);
            loader.removeAllJobs (true, 3000);
        }

        //==============================================================================
        enum class Learn { none, reference, current };

        void setLearning (Learn l)
        {
            learning = l;
            lastWrite = proc.preScope.writePos.load();
            const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
            if (l == Learn::reference && (refFromFile || ! ref->hasData())) { ref->reset (sr); refFromFile = false; refName = "Learned from input"; }
            if (l == Learn::current && ! cur->hasData()) cur->reset (sr);
            changed();
        }

        Learn getLearning() const noexcept { return learning; }

        void clearReference() { ref = std::make_shared<SpectrumAccumulator>(); refName.clear(); refFromFile = false; if (learning == Learn::reference) learning = Learn::none; changed(); }
        void clearCurrent()   { cur = std::make_shared<SpectrumAccumulator>(); if (learning == Learn::current) learning = Learn::none; changed(); }

        bool hasReference() const noexcept { return ref->hasData(); }
        bool hasCurrent() const noexcept { return cur->hasData(); }
        double referenceSeconds() const noexcept { return ref->getSeconds(); }
        double currentSeconds() const noexcept { return cur->getSeconds(); }
        const juce::String& referenceName() const noexcept { return refName; }
        bool isLoadingFile() const noexcept { return loading; }

        void loadReferenceFile (const juce::File& f)
        {
            loading = true;
            if (learning == Learn::reference) learning = Learn::none;
            refName = f.getFileName();
            auto aliveFlag = alive;
            loader.addJob ([this, f, aliveFlag]
            {
                auto acc = std::make_shared<SpectrumAccumulator>();
                juce::String error;
                {
                    juce::AudioFormatManager fm;
                    fm.registerBasicFormats();
                    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
                    if (r == nullptr) error = "Can't read this file";
                    else
                    {
                        acc->reset (r->sampleRate);
                        const juce::int64 total = juce::jmin (r->lengthInSamples, (juce::int64) (r->sampleRate * 60 * 15));
                        const int chunk = (int) r->sampleRate * 20;
                        for (juce::int64 pos = 0; pos < total && aliveFlag->load(); pos += chunk)
                        {
                            const int n = (int) juce::jmin ((juce::int64) chunk + SpectrumAccumulator::size, total - pos);
                            juce::AudioBuffer<float> b ((int) juce::jmin (2u, r->numChannels), n);
                            r->read (&b, 0, n, pos, true, b.getNumChannels() > 1);
                            acc->addBuffer (b);
                        }
                        if (! acc->hasData()) error = "The file is silent";
                    }
                }
                juce::MessageManager::callAsync ([this, aliveFlag, acc, error, name = f.getFileName()]
                {
                    if (! aliveFlag->load()) return;
                    loading = false;
                    if (error.isEmpty())
                    {
                        ref = acc;
                        refName = name;
                        refFromFile = true;
                    }
                    else refName = error;
                    changed();
                });
            });
            changed();
        }

        // settings
        void setAmount (double a) { amount = a; changed(); }
        void setBands (int b) { bands = b; changed(); }
        void setSmoothing (int index) { smoothingIndex = index; changed(); }
        void setReplace (bool r) { replace = r; }
        double getAmount() const noexcept { return amount; }
        int getBands() const noexcept { return bands; }
        int getSmoothing() const noexcept { return smoothingIndex; }
        bool getReplace() const noexcept { return replace; }

        // call at ~60 Hz from the UI
        void tick()
        {
            if (learning == Learn::none) return;
            auto& acc = learning == Learn::reference ? *ref : *cur;
            const int w = proc.preScope.writePos.load();
            const int fresh = (w - lastWrite) & (ScopeRing::size - 1);
            if (fresh < SpectrumAccumulator::size / 2) return;
            frame.resize ((size_t) SpectrumAccumulator::size);
            proc.preScope.readLatest (frame.data(), SpectrumAccumulator::size);
            const double sr = acc.getSampleRate();
            const bool hadData = acc.hasData();
            acc.addFrame (frame.data(), fresh / sr);
            lastWrite = w;
            if (! hadData || ++learnTicks % 8 == 0) changed();
        }

        //==============================================================================
        // Data for drawing (message thread)
        const std::vector<double>& freqs() const noexcept { return grid; }
        const std::vector<double>& referenceDb() const noexcept { return refDb; }
        const std::vector<double>& currentDb() const noexcept { return curDb; }
        const std::vector<double>& targetDb() const noexcept { return target; }
        const std::vector<double>& previewDb() const noexcept { return preview; }
        const std::vector<BandSettings>& fittedBands() const noexcept { return fitted; }
        double levelOffset() const noexcept { return offset; }
        bool isFitting() const noexcept { return fitting; }
        int version() const noexcept { return ver; }

        bool canApply() const noexcept { return hasReference() && hasCurrent() && ! fitted.empty() && ! fitting; }

        void apply()
        {
            if (! canApply()) return;
            proc.undo.beginNewTransaction ("Match EQ");
            if (replace)
                for (int b = 0; b < numBands; ++b)
                    if (proc.band[(size_t) b].used->load() > 0.5f)
                    {
                        proc.setParam (bandId (b, "used"), 0.0f);
                        proc.resetBand (b);
                    }

            for (const auto& s : fitted)
            {
                int slot = -1;
                for (int b = 0; b < numBands; ++b) if (proc.band[(size_t) b].used->load() < 0.5f) { slot = b; break; }
                if (slot < 0) break;
                proc.resetBand (slot);
                proc.setParam (bandId (slot, "type"), (float) s.type);
                proc.setParam (bandId (slot, "freq"), (float) s.freq);
                proc.setParam (bandId (slot, "gain"), (float) s.gainDb);
                proc.setParam (bandId (slot, "q"), (float) s.q);
                proc.setParam (bandId (slot, "used"), 1.0f);
            }
            proc.currentPresetName = "Match EQ";
        }

        static double smoothingOctaves (int index)
        {
            static const double v[] { 1.0 / 12.0, 1.0 / 6.0, 1.0 / 3.0, 1.0 / 2.0, 1.0 };
            return v[juce::jlimit (0, 4, index)];
        }

    private:
        void changed()
        {
            const double sm = smoothingOctaves (smoothingIndex);
            refDb = ref->hasData() ? ref->toGrid (grid, sm) : std::vector<double>();
            curDb = cur->hasData() ? cur->toGrid (grid, sm) : std::vector<double>();

            target.clear();
            preview.clear();
            if (! refDb.empty() && ! curDb.empty())
            {
                std::vector<double> diff (grid.size());
                double s = 0, w = 0;
                for (size_t i = 0; i < grid.size(); ++i)
                {
                    diff[i] = refDb[i] - curDb[i];
                    if (grid[i] >= 100.0 && grid[i] <= 8000.0) { s += diff[i]; w += 1.0; }
                }
                offset = w > 0 ? s / w : 0.0;
                target.resize (grid.size());
                for (size_t i = 0; i < grid.size(); ++i)
                    target[i] = juce::jlimit (-18.0, 18.0, diff[i] - offset) * amount;

                // ask the fitter thread for new bands (latest request wins)
                {
                    const juce::ScopedLock sl (lock);
                    pendingTarget = target;
                    pendingBands = bands;
                    ++requestId;
                }
                cancelFit = true;
                fitting = true;
                notify();
            }
            else
            {
                fitted.clear();
                fitting = false;
            }
            ++ver;
        }

        void run() override
        {
            int done = 0;
            while (! threadShouldExit())
            {
                wait (-1);
                while (! threadShouldExit())
                {
                    std::vector<double> t;
                    int nb, id;
                    {
                        const juce::ScopedLock sl (lock);
                        if (requestId == done) break;
                        t = pendingTarget; nb = pendingBands; id = requestId;
                    }
                    cancelFit = false;
                    FitOptions o;
                    o.maxBands = nb;
                    o.maxGain = 18.0;
                    o.minQ = 0.3;
                    o.maxQ = 4.0;
                    auto res = CurveFitter::fit (grid, t, weights, o, &cancelFit);
                    done = id;
                    if (cancelFit.load()) continue;

                    auto aliveFlag = alive;
                    juce::MessageManager::callAsync ([this, aliveFlag, res, id]
                    {
                        if (! aliveFlag->load() || id != requestId) return;
                        fitted = res.bands;
                        preview = CurveFitter::response (grid, fitted);
                        fitting = false;
                        ++ver;
                    });
                }
            }
        }

        AvelProcessor& proc;
        std::vector<double> grid, weights;
        std::shared_ptr<SpectrumAccumulator> ref, cur;
        juce::String refName;
        bool refFromFile = false;
        std::atomic<bool> loading { false };
        Learn learning = Learn::none;
        int lastWrite = 0, learnTicks = 0;
        std::vector<float> frame;

        double amount = 1.0, offset = 0.0;
        int bands = 8, smoothingIndex = 2;
        bool replace = true;

        std::vector<double> refDb, curDb, target, preview;
        std::vector<BandSettings> fitted;
        bool fitting = false;
        int ver = 0;

        juce::CriticalSection lock;
        std::vector<double> pendingTarget;
        int pendingBands = 8;
        int requestId = 0;
        std::atomic<bool> cancelFit { false };

        juce::ThreadPool loader { 1 };
        std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);
    };
}
