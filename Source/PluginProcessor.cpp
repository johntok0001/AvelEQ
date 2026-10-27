#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace avel;

//==============================================================================
class AvelProcessor::KernelThread : public juce::Thread
{
public:
    explicit KernelThread (AvelProcessor& p) : juce::Thread ("Avel EQ linear phase"), proc (p) {}

    void forceRebuild() noexcept { force.store (true); }

    void run() override
    {
        uint64_t built = 0;
        while (! threadShouldExit())
        {
            wait (15);
            if (! proc.linearMode.load())
                continue;

            const uint64_t want = proc.wantedHash.load() ^ ((uint64_t) proc.firMode.load() << 60);
            if (want == built && ! force.exchange (false))
                continue;

            const auto snap = proc.snapshot();
            const int length = LinearPhaseEngine::lengthForResolution ((int) proc.apvts.getRawParameterValue ("lpRes")->load());
            {
                const juce::ScopedLock sl (proc.engineLock);
                proc.linear.build (snap, length, proc.firMode.load() == 1);
            }
            built = want;
        }
    }

private:
    AvelProcessor& proc;
    std::atomic<bool> force { true };
};

//==============================================================================
AvelProcessor::AvelProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, &undo, "PARAMS", createLayout())
{
    for (int b = 0; b < numBands; ++b)
    {
        band[(size_t) b].attach (apvts, b);
        dynGainNow[(size_t) b].store (0.0f);
        dynActive[(size_t) b].store (false);
    }
    for (auto& p : outPeak) p.store (0.0f);

    apvts.addParameterListener ("phase", this);
    apvts.addParameterListener ("lpRes", this);

    uiState.setProperty ("analyzer", 0, nullptr);
    uiState.setProperty ("range", 12, nullptr);
    uiState.setProperty ("width", 1180, nullptr);
    uiState.setProperty ("height", 720, nullptr);

    kernelThread = std::make_unique<KernelThread> (*this);
    kernelThread->startThread (juce::Thread::Priority::low);
}

AvelProcessor::~AvelProcessor()
{
    cancelPendingUpdate();
    apvts.removeParameterListener ("phase", this);
    apvts.removeParameterListener ("lpRes", this);
    kernelThread->signalThreadShouldExit();
    kernelThread->notify();
    kernelThread->stopThread (3000);
}

bool AvelProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::stereo() && out != juce::AudioChannelSet::mono())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

//==============================================================================
void AvelProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    sr = sampleRate > 0 ? sampleRate : 44100.0;

    for (auto& r : rt)
    {
        r.initialised = false;
        r.mix = 0.0f;
        r.resetState();
    }
    for (auto& ch : soloSt) for (auto& s : ch) s.reset();
    lastSolo = -2;

    outGainSmoothed.reset (sr, 0.05);
    outGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (apvts.getRawParameterValue ("outGain")->load()));

    {
        const juce::ScopedLock sl (engineLock);
        linear.prepare (sr, juce::jmax (samplesPerBlock, 32));
    }
    linearMode.store (isLinearPhase());
    firMode.store (getPhaseMode());
    lastLinear = linearMode.load();
    kernelThread->forceRebuild();
    wantedHash.store (snapshotHash());

    setLatencySamples (latencyForCurrentMode());
    anyRedesign = true;
}

void AvelProcessor::parameterChanged (const juce::String&, float)
{
    triggerAsyncUpdate();
}

void AvelProcessor::handleAsyncUpdate()
{
    linearMode.store (isLinearPhase());
    firMode.store (getPhaseMode());
    kernelThread->forceRebuild();
    setLatencySamples (latencyForCurrentMode());
}

int AvelProcessor::latencyForCurrentMode() const noexcept
{
    const int L = LinearPhaseEngine::lengthForResolution ((int) apvts.getRawParameterValue ("lpRes")->load());
    switch (getPhaseMode())
    {
        case 1:  return LinearPhaseEngine::naturalDelay (L);
        case 2:  return L / 2;
        default: return 0;
    }
}

//==============================================================================
EQSnapshot AvelProcessor::snapshot() const
{
    EQSnapshot s;
    for (int b = 0; b < numBands; ++b)
    {
        const auto& p = band[(size_t) b];
        s[(size_t) b].active = p.isActive();
        s[(size_t) b].s = p.settings();
        s[(size_t) b].placement = p.placement();
    }
    return s;
}

uint64_t AvelProcessor::snapshotHash() const
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h] (float v)
    {
        uint32_t bits;
        std::memcpy (&bits, &v, sizeof (bits));
        h = (h ^ bits) * 1099511628211ull;
    };

    for (const auto& p : band)
    {
        const bool active = p.isActive();
        mix (active ? 1.0f : 0.0f);
        if (! active) continue;
        mix (p.type->load()); mix (p.freq->load()); mix (p.gain->load());
        mix (p.q->load()); mix (p.slope->load()); mix (p.stereo->load());
    }
    mix ((float) sr);
    return h;
}

//==============================================================================
void AvelProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n = buffer.getNumSamples();
    const int numCh = juce::jmin (2, buffer.getNumChannels());
    if (n == 0 || numCh == 0)
        return;

    for (int ch = numCh; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, n);

    const bool stereo = numCh > 1;
    float* L = buffer.getWritePointer (0);
    float* R = stereo ? buffer.getWritePointer (1) : nullptr;

    preScope.push (L, R, n);

    const uint64_t hash = snapshotHash();
    const bool hashChanged = hash != wantedHash.load();
    wantedHash.store (hash);

    const bool lin = linearMode.load();
    if (lin != lastLinear)
    {
        for (auto& r : rt) { r.initialised = false; r.mix = 0.0f; r.resetState(); }
        linear.reset();
        lastLinear = lin;
    }

    if (lin)
    {
        for (int b = 0; b < numBands; ++b)
            dynActive[(size_t) b].store (false);

        if (linear.isBuilt())
        {
            if (stereo)
                linear.process (L, R, n);
            else
            {
                // mono: feed the same signal to both inputs, keep the left output
                float tmpR[512];
                for (int pos = 0; pos < n; pos += 512)
                {
                    const int m = juce::jmin (512, n - pos);
                    juce::FloatVectorOperations::copy (tmpR, L + pos, m);
                    linear.process (L + pos, tmpR, m);
                }
            }
        }
    }
    else
    {
        processZeroLatency (L, R, n, stereo);
    }

    applySolo (L, R, n, stereo);

    if (hashChanged || anyRedesign)
    {
        updateAutoGain (n);
        anyRedesign = false;
    }

    const bool autoOn = apvts.getRawParameterValue ("autoGain")->load() > 0.5f;
    autoGainNow.store (autoOn ? autoGainTarget : 0.0f);
    const float outDb = apvts.getRawParameterValue ("outGain")->load() + (autoOn ? autoGainTarget : 0.0f);
    outGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (outDb));

    if (outGainSmoothed.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = outGainSmoothed.getNextValue();
            L[i] *= g;
            if (stereo) R[i] *= g;
        }
    }
    else
    {
        const float g = outGainSmoothed.getCurrentValue();
        juce::FloatVectorOperations::multiply (L, g, n);
        if (stereo) juce::FloatVectorOperations::multiply (R, g, n);
    }

    postScope.push (L, R, n);

    auto peakOf = [n] (const float* d)
    {
        auto r = juce::FloatVectorOperations::findMinAndMax (d, n);
        return juce::jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
    };
    auto storeMax = [] (std::atomic<float>& a, float v)
    {
        auto prev = a.load();
        while (v > prev && ! a.compare_exchange_weak (prev, v)) {}
    };
    storeMax (outPeak[0], peakOf (L));
    storeMax (outPeak[1], stereo ? peakOf (R) : peakOf (L));
}

void AvelProcessor::processZeroLatency (float* L, float* R, int n, bool stereo)
{
    for (int b = 0; b < numBands; ++b)
    {
        auto& r = rt[(size_t) b];
        const bool active = band[(size_t) b].isActive();

        if (! active && r.mix <= 0.0f)
        {
            if (r.initialised) { r.initialised = false; r.resetState(); }
            dynActive[(size_t) b].store (false);
            continue;
        }

        processBand (b, r, L, R, n, stereo, active ? 1.0f : 0.0f);
    }
}

static BandDesign detectorFor (const BandSettings& s, double sr)
{
    BandSettings d;
    d.freq = s.freq;
    d.q = s.q;
    d.slopeIndex = 1;
    switch (s.type)
    {
        case FilterType::lowShelf:  d.type = FilterType::highCut; d.q = 0.7071; break;
        case FilterType::highShelf: d.type = FilterType::lowCut;  d.q = 0.7071; break;
        case FilterType::tiltShelf: return {};
        default:                    d.type = FilterType::bandPass; break;
    }
    return designBand (d, sr);
}

void AvelProcessor::processBand (int b, BandRuntime& r, float* L, float* R, int n, bool stereo, float targetMix)
{
    const auto& p = band[(size_t) b];
    const auto target = p.settings();
    const auto placement = p.placement();
    const bool dynamic = p.isDynamic();

    bool redesign = false;

    if (! r.initialised)
    {
        r.cur = target;
        r.resetState();
        r.initialised = true;
        r.mix = 0.0f;
        r.placement = placement;
        redesign = true;
    }

    if (placement != r.placement)
    {
        r.placement = placement;
        r.resetState();
    }

    if (target.type != r.cur.type || target.slopeIndex != r.cur.slopeIndex)
    {
        r.cur.type = target.type;
        r.cur.slopeIndex = target.slopeIndex;
        redesign = true;
    }

    // parameter smoothing (~15 ms), frequency and Q in the log domain
    const double alpha = 1.0 - std::exp (-(double) n / (0.015 * sr));
    auto smoothLog = [&] (double& cur, double tgt, double eps)
    {
        const double lc = std::log (cur), lt = std::log (tgt);
        if (std::abs (lc - lt) > eps) { cur = std::exp (lc + (lt - lc) * alpha); redesign = true; }
        else if (cur != tgt) { cur = tgt; redesign = true; }
    };
    smoothLog (r.cur.freq, target.freq, 1.0e-4);
    smoothLog (r.cur.q, target.q, 1.0e-4);
    if (std::abs (r.cur.gainDb - target.gainDb) > 1.0e-3) { r.cur.gainDb += (target.gainDb - r.cur.gainDb) * alpha; redesign = true; }
    else if (r.cur.gainDb != target.gainDb) { r.cur.gainDb = target.gainDb; redesign = true; }

    if (redesign)
    {
        anyRedesign = true;
        auto det = detectorFor (r.cur, sr);
        r.detectSections = det.numSections;
        for (int i = 0; i < det.numSections && i < 2; ++i)
            r.detect[i] = det.sections[(size_t) i];
    }

    if (! dynamic && (redesign || r.wasDynamic))
        r.design = designBand (r.cur, sr);
    r.wasDynamic = dynamic;

    dynActive[(size_t) b].store (dynamic);
    if (! dynamic)
        dynGainNow[(size_t) b].store ((float) r.cur.gainDb);

    const float range = p.range->load();
    const float thr = p.thr->load();
    const double atk = 1.0 - std::exp (-1.0 / (0.005 * sr));
    const double rel = 1.0 - std::exp (-1.0 / (0.080 * sr));
    const float mixStep = 1.0f / (float) (0.02 * sr);

    const int sub = dynamic ? 32 : n;
    const int ns = r.design.numSections;

    for (int pos = 0; pos < n; pos += sub)
    {
        const int m = juce::jmin (sub, n - pos);
        float* l = L + pos;
        float* rr = R != nullptr ? R + pos : nullptr;

        if (dynamic)
        {
            // level of the band's own region, measured on its input
            for (int i = 0; i < m; ++i)
            {
                double a = l[i], c = stereo ? rr[i] : l[i];
                switch (placement)
                {
                    case Placement::mid:  a = 0.5 * (a + c); c = a; break;
                    case Placement::side: a = 0.5 * (a - c); c = a; break;
                    case Placement::left: c = a; break;
                    case Placement::right: a = c; break;
                    case Placement::stereo: break;
                }
                juce::ignoreUnused (c);
                for (int k = 0; k < r.detectSections; ++k)
                    a = r.detSt[k].process (r.detect[k], a);
                const double pw = a * a;
                r.env += (pw > r.env ? atk : rel) * (pw - r.env);
            }

            const double levelDb = 10.0 * std::log10 (r.env + 1.0e-20) + 3.0103;
            const double over = levelDb - thr;
            double amount = 0.0;
            if (over > 0.0)
            {
                const double x = juce::jmin (1.0, over / 18.0);
                amount = x * x * (3.0 - 2.0 * x);
            }
            BandSettings ds = r.cur;
            ds.gainDb = juce::jlimit (-30.0, 30.0, r.cur.gainDb + range * amount);
            r.design = designBand (ds, sr);
            dynGainNow[(size_t) b].store ((float) ds.gainDb);
        }

        auto run = [&] (int ch, double x) noexcept
        {
            auto& s = r.st[(size_t) ch];
            for (int k = 0; k < ns; ++k)
                x = s[(size_t) k].process (r.design.sections[(size_t) k], x);
            return x;
        };

        for (int i = 0; i < m; ++i)
        {
            if (r.mix != targetMix)
                r.mix = targetMix > r.mix ? juce::jmin (targetMix, r.mix + mixStep) : juce::jmax (targetMix, r.mix - mixStep);
            const float w = r.mix;

            const float xl = l[i];
            const float xr = stereo ? rr[i] : xl;
            float yl = xl, yr = xr;

            switch (placement)
            {
                case Placement::stereo:
                    yl = (float) run (0, xl);
                    if (stereo) yr = (float) run (1, xr);
                    break;
                case Placement::left:
                    yl = (float) run (0, xl);
                    break;
                case Placement::right:
                    if (stereo) yr = (float) run (1, xr);
                    break;
                case Placement::mid:
                {
                    const double mid = 0.5 * (xl + xr), side = 0.5 * (xl - xr);
                    const double fm = run (0, mid);
                    yl = (float) (fm + side);
                    yr = (float) (fm - side);
                    break;
                }
                case Placement::side:
                {
                    if (! stereo) break;
                    const double mid = 0.5 * (xl + xr), side = 0.5 * (xl - xr);
                    const double fs = run (0, side);
                    yl = (float) (mid + fs);
                    yr = (float) (mid - fs);
                    break;
                }
            }

            if (w >= 1.0f)
            {
                l[i] = yl;
                if (stereo) rr[i] = yr;
            }
            else
            {
                l[i] = xl + w * (yl - xl);
                if (stereo) rr[i] = xr + w * (yr - xr);
            }
        }
    }

    if (r.mix <= 0.0f && targetMix <= 0.0f)
    {
        r.initialised = false;
        r.resetState();
    }
}

void AvelProcessor::applySolo (float* L, float* R, int n, bool stereo)
{
    const int s = soloBand.load();
    if (s < 0 || s >= numBands || band[(size_t) s].used->load() < 0.5f)
    {
        lastSolo = -1;
        return;
    }

    if (s != lastSolo)
    {
        for (auto& ch : soloSt) for (auto& st : ch) st.reset();
        lastSolo = s;
    }

    const auto bs = band[(size_t) s].settings();
    BandSettings d;
    d.freq = bs.freq;
    d.slopeIndex = 1;
    d.q = juce::jmax (0.5, bs.q);
    switch (bs.type)
    {
        case FilterType::lowCut:
        case FilterType::lowShelf:  d.type = FilterType::highCut; d.q = 0.7071; break;
        case FilterType::highCut:
        case FilterType::highShelf: d.type = FilterType::lowCut;  d.q = 0.7071; break;
        case FilterType::tiltShelf: d.type = FilterType::bandPass; d.q = 0.5; break;
        default:                    d.type = FilterType::bandPass; break;
    }
    soloDesign = designBand (d, sr);

    for (int ch = 0; ch < (stereo ? 2 : 1); ++ch)
    {
        float* x = ch == 0 ? L : R;
        auto& st = soloSt[(size_t) ch];
        for (int i = 0; i < n; ++i)
        {
            double v = x[i];
            for (int k = 0; k < soloDesign.numSections; ++k)
                v = st[(size_t) k].process (soloDesign.sections[(size_t) k], v);
            x[i] = (float) v;
        }
    }
}

void AvelProcessor::updateAutoGain (int)
{
    // Loudness compensation: average power gain of the static curve, equal weight per octave
    constexpr int points = 48;
    std::array<BandDesign, numBands> designs;
    std::array<float, numBands> weight {};

    for (int b = 0; b < numBands; ++b)
    {
        const auto& p = band[(size_t) b];
        if (! p.isActive()) continue;
        designs[(size_t) b] = designBand (p.settings(), sr);
        weight[(size_t) b] = p.placement() == Placement::stereo ? 1.0f : 0.5f;
    }

    double sum = 0.0;
    for (int i = 0; i < points; ++i)
    {
        const double f = 20.0 * std::pow (1000.0, (i + 0.5) / points);
        if (f >= 0.49 * sr) { sum += 1.0; continue; }
        const double w = 2.0 * pi * f / sr;
        double db = 0.0;
        for (int b = 0; b < numBands; ++b)
            if (weight[(size_t) b] > 0.0f)
                db += weight[(size_t) b] * designs[(size_t) b].magDb (w);
        sum += std::pow (10.0, db / 10.0);
    }

    autoGainTarget = (float) juce::jlimit (-24.0, 24.0, -10.0 * std::log10 (sum / points));
}

//==============================================================================
int AvelProcessor::addBand (FilterType type, float freq, float gainDb, float q)
{
    for (int b = 0; b < numBands; ++b)
    {
        if (band[(size_t) b].used->load() > 0.5f)
            continue;

        undo.beginNewTransaction ("Add band");
        resetBand (b);
        setParam (bandId (b, "type"), (float) type);
        setParam (bandId (b, "freq"), freq);
        setParam (bandId (b, "gain"), typeHasGain (type) ? gainDb : 0.0f);
        setParam (bandId (b, "q"), q);
        setParam (bandId (b, "used"), 1.0f);
        return b;
    }
    return -1;
}

void AvelProcessor::resetBand (int b)
{
    for (const char* id : { "on", "type", "freq", "gain", "q", "slope", "stereo", "dyn", "range", "thr" })
        if (auto* p = param (bandId (b, id)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
}

void AvelProcessor::removeBand (int b)
{
    undo.beginNewTransaction ("Remove band");
    if (soloBand.load() == b) soloBand.store (-1);
    setParam (bandId (b, "used"), 0.0f);
    resetBand (b);
}

void AvelProcessor::setParam (const juce::String& id, float plainValue)
{
    if (auto* p = param (id))
    {
        p->beginChangeGesture();
        p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
        p->endChangeGesture();
    }
}

//==============================================================================
void AvelProcessor::switchAB (int slot)
{
    if (slot == abSlot) return;
    abStates[abSlot] = apvts.copyState();
    if (abStates[slot].isValid())
        apvts.replaceState (abStates[slot].createCopy());
    abSlot = slot;
    undo.clearUndoHistory();
}

void AvelProcessor::copyABToOther()
{
    abStates[1 - abSlot] = apvts.copyState();
}

//==============================================================================
namespace
{
    struct PresetBand
    {
        FilterType type; float freq, gain, q; int slope = 1; Placement pl = Placement::stereo;
        bool dyn = false; float range = -6.0f, thr = -24.0f;
    };

    struct Preset { const char* name; std::vector<PresetBand> bands; };

    const std::vector<Preset>& factoryPresets()
    {
        using T = FilterType;
        static const std::vector<Preset> p {
            { "Default", {} },
            { "Vocal Presence", { { T::lowCut, 90, 0, 0.71f, 3 }, { T::bell, 300, -2.5f, 1.2f }, { T::bell, 3200, 2.5f, 0.9f },
                                  { T::bell, 7000, 0, 3.0f, 1, Placement::stereo, true, -5.0f, -30.0f }, { T::highShelf, 12000, 2.0f, 0.71f } } },
            { "Kick & Bass Clean", { { T::lowCut, 28, 0, 0.71f, 5 }, { T::bell, 60, 2.0f, 1.4f }, { T::bell, 250, -3.5f, 1.6f },
                                     { T::bell, 2500, 1.5f, 1.0f } } },
            { "Air & Sparkle", { { T::highShelf, 10000, 3.0f, 0.6f }, { T::bell, 5000, -1.0f, 1.5f } } },
            { "Master Smile", { { T::lowCut, 20, 0, 0.71f, 2 }, { T::lowShelf, 90, 1.5f, 0.6f }, { T::bell, 450, -1.0f, 0.8f },
                                { T::highShelf, 9000, 1.5f, 0.6f } } },
            { "De-Mud", { { T::bell, 220, -3.0f, 1.0f }, { T::bell, 450, -2.0f, 1.4f } } },
            { "Wide Highs (M/S)", { { T::highShelf, 6000, 2.5f, 0.71f, 1, Placement::side }, { T::lowCut, 150, 0, 0.71f, 3, Placement::side } } },
            { "Dynamic De-Ess", { { T::bell, 6500, 0, 2.5f, 1, Placement::stereo, true, -9.0f, -32.0f } } },
            { "Telephone", { { T::lowCut, 400, 0, 0.71f, 3 }, { T::highCut, 3200, 0, 0.71f, 3 }, { T::bell, 1500, 4.0f, 1.0f } } },
        };
        return p;
    }
}

juce::StringArray AvelProcessor::getFactoryPresetNames()
{
    juce::StringArray s;
    for (auto& p : factoryPresets()) s.add (p.name);
    return s;
}

void AvelProcessor::loadFactoryPreset (int index)
{
    const auto& all = factoryPresets();
    if (index < 0 || index >= (int) all.size()) return;

    undo.beginNewTransaction ("Load preset");
    for (int b = 0; b < numBands; ++b)
    {
        setParam (bandId (b, "used"), 0.0f);
        resetBand (b);
    }

    int b = 0;
    for (const auto& pb : all[(size_t) index].bands)
    {
        setParam (bandId (b, "type"), (float) pb.type);
        setParam (bandId (b, "freq"), pb.freq);
        setParam (bandId (b, "gain"), pb.gain);
        setParam (bandId (b, "q"), pb.q);
        setParam (bandId (b, "slope"), (float) pb.slope);
        setParam (bandId (b, "stereo"), (float) pb.pl);
        setParam (bandId (b, "dyn"), pb.dyn ? 1.0f : 0.0f);
        setParam (bandId (b, "range"), pb.range);
        setParam (bandId (b, "thr"), pb.thr);
        setParam (bandId (b, "used"), 1.0f);
        ++b;
    }
    setParam ("outGain", 0.0f);
    currentPresetName = all[(size_t) index].name;
}

juce::File AvelProcessor::getUserPresetFolder()
{
    auto f = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Avel EQ").getChildFile ("Presets");
    f.createDirectory();
    return f;
}

bool AvelProcessor::saveUserPreset (const juce::File& file)
{
    auto xml = apvts.copyState().createXml();
    if (xml == nullptr) return false;
    xml->setTagName ("AvelPreset");
    if (! xml->writeTo (file)) return false;
    currentPresetName = file.getFileNameWithoutExtension();
    return true;
}

bool AvelProcessor::loadUserPreset (const juce::File& file)
{
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName ("AvelPreset")) return false;
    xml->setTagName (apvts.state.getType().toString());
    undo.beginNewTransaction ("Load preset");
    apvts.replaceState (juce::ValueTree::fromXml (*xml));
    currentPresetName = file.getFileNameWithoutExtension();
    return true;
}

//==============================================================================
void AvelProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("AvelEQ");
    root.setProperty ("version", 1, nullptr);
    root.setProperty ("preset", currentPresetName, nullptr);
    root.setProperty ("abSlot", abSlot, nullptr);
    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (uiState.createCopy(), nullptr);

    for (int i = 0; i < 2; ++i)
        if (i != abSlot && abStates[i].isValid())
        {
            juce::ValueTree ab ("AB" + juce::String (i));
            ab.appendChild (abStates[i].createCopy(), nullptr);
            root.appendChild (ab, nullptr);
        }

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void AvelProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr) return;
    auto root = juce::ValueTree::fromXml (*xml);
    if (! root.hasType ("AvelEQ")) return;

    if (auto p = root.getChildWithName (apvts.state.getType()); p.isValid())
        apvts.replaceState (p);

    if (auto u = root.getChildWithName ("UI"); u.isValid())
        uiState.copyPropertiesFrom (u, nullptr);

    currentPresetName = root.getProperty ("preset", "Default").toString();
    abSlot = juce::jlimit (0, 1, (int) root.getProperty ("abSlot", 0));
    for (int i = 0; i < 2; ++i)
    {
        abStates[i] = {};
        if (auto ab = root.getChildWithName ("AB" + juce::String (i)); ab.isValid() && ab.getNumChildren() > 0)
            abStates[i] = ab.getChild (0).createCopy();
    }

    undo.clearUndoHistory();
    triggerAsyncUpdate();
}

//==============================================================================
juce::AudioProcessorEditor* AvelProcessor::createEditor()
{
    return new AvelEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AvelProcessor();
}
